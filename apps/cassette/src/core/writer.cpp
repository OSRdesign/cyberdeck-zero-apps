/*
 * SPDX-License-Identifier: MIT
 */
#include "writer.hpp"

#include "codec/codec_libs.hpp"
#include "codec/stream_scan.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>

namespace cassette {
namespace {

std::string dir_of(const std::string &p) {
    size_t s = p.rfind('/');
    if (s == std::string::npos) return ".";
    return s == 0 ? "/" : p.substr(0, s);
}

/* An error code as text: errno values as strerror, the codec layer's own values by name (err > 0). */
std::string error_text(int err) { return codec_error_text(-err); }

}  // namespace

Writer::Writer(Ring &ring, FileOps &ops, Clock &clk, WriterConfig cfg)
    : ring_(ring), ops_(ops), clk_(clk), cfg_(std::move(cfg)) {
    ba_ = block_align(cfg_.fmt);
    fb_ = ring_.frame_bytes();
    if (cfg_.sync_interval_ms) {
        sync_ms_ = cfg_.sync_interval_ms;
    } else {
        sync_ms_ = std::min<uint32_t>(30, std::max<uint32_t>(5, cfg_.sync_interval_s)) * 1000u;
    }
    cfg_.block_bytes = std::max<size_t>(cfg_.block_bytes, fb_);
}

Writer::~Writer() { stop(); }

int Writer::start() {
    if (state_ != WriterState::Idle) return -EBUSY;
    if (!ba_ || ba_ != fb_ || !cfg_.namer) return -EINVAL;
    const uint64_t now = clk_.mono_ms();
    uint64_t avail = 0;
    const std::string first = cfg_.namer(0);
    if (ops_.free_space(dir_of(first), avail) == 0 && avail < cfg_.min_free_bytes) {
        state_ = WriterState::Failed;
        reason_ = StopReason::LowSpace;
        reason_text_ = "not enough free space";
        error_ = ENOSPC;
        publish();
        return -ENOSPC;
    }
    je_.take = cfg_.take;
    je_.fmt = cfg_.fmt;
    je_.device_name = cfg_.device_name;
    je_.device_id = cfg_.device_id;
    je_.started_mono_ms = static_cast<int64_t>(now);
    je_.started_wall = clk_.wall_s();
    je_.sync_interval_ms = sync_ms_;
    je_.auto_split = cfg_.auto_split;
    if (cfg_.make_encoder) {
        je_.codec = cfg_.codec;
        je_.codec_preset = cfg_.codec_preset;
        je_.bitrate_bps = cfg_.bitrate_bps;
        je_.force_mono = cfg_.force_mono;
        je_.encoder = cfg_.encoder_name;
    }
    int r = open_part(0);
    if (r < 0) {
        state_ = WriterState::Failed;
        reason_ = StopReason::IoError;
        error_ = -r;
        reason_text_ = std::string("cannot start the file: ") + error_text(-r);
        publish();
        return r;
    }
    last_sync_ms_ = last_write_ms_ = last_space_ms_ = now;
    state_ = WriterState::Recording;
    if (!cfg_.recordings_dir.empty()) {
        r = journal_update("open", "");
        if (r < 0) {
            abandon_file();
            ops_.unlink(path_);
            state_ = WriterState::Failed;
            reason_ = StopReason::IoError;
            error_ = -r;
            reason_text_ = std::string("cannot write the journal: ") + std::strerror(-r);
            publish();
            return r;
        }
    }
    publish();
    return 0;
}

int Writer::open_encoder(int fd, int part) {
    enc_ = cfg_.make_encoder(part);
    if (!enc_) return kCodecMissing;
    sink_.reset(new FileByteSink(ops_, fd, 0));
    const Tags tags = cfg_.tags ? cfg_.tags(part) : Tags();
    return enc_->start(*sink_, tags); /* the stream headers: an empty take is a valid file */
}

int Writer::open_part(int part) {
    const std::string path = cfg_.namer(part);
    int fd = ops_.open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return fd;
    int r;
    if (cfg_.make_encoder) {
        r = open_encoder(fd, part);
    } else {
        BwfInfo b = cfg_.bwf;
        b.time_reference += total_bytes_ / (fb_ ? fb_ : 1); /* a later part continues where the last one ended */
        std::vector<unsigned char> h = build_header(cfg_.fmt, b, 0, false);
        r = write_all(ops_, fd, h.data(), h.size());
    }
    if (r == 0) r = ops_.fsync(fd);
    if (r == 0) r = ops_.fsync_dir(dir_of(path)); /* the file exists after a power cut */
    if (r < 0) {
        enc_.reset(); /* before its sink; an unfinished encoder writes nothing more */
        sink_.reset();
        ops_.close(fd);
        ops_.unlink(path);
        return r;
    }
    fd_ = fd;
    path_ = path;
    part_ = part;
    part_data_ = 0;
    part_frames_ = 0;
    unsynced_ = 0;
    rf64_ = false;
    if (je_.parts.size() <= static_cast<size_t>(part)) je_.parts.push_back(path);
    je_.path = je_.parts[0];
    return 0;
}

int Writer::apply_patches(const PatchSet &ps) {
    for (const Patch &p : ps.patches) {
        ssize_t w = ops_.pwrite(fd_, p.bytes.data(), p.bytes.size(), p.offset);
        if (w < 0) return static_cast<int>(w);
        if (static_cast<size_t>(w) != p.bytes.size()) return -EIO;
    }
    return 0;
}

int Writer::switch_rf64() {
    HeaderLayout l;
    l.block_align = ba_;
    PatchSet ps = size_patches(l, part_data_, true);
    int r = apply_patches(ps);
    if (r == 0) rf64_ = ps.rf64;
    return r;
}

int Writer::finalize_part() {
    if (enc_) {
        /* the stream's end (FLAC: the last partial block, then STREAMINFO rewritten; Opus: the decision delay drained
         * and the end-of-stream page), then the data is made durable. On an error the descriptor stays open: fail()
         * mends the file and closes it. */
        int r = enc_->finish();
        if (r == 0) r = ops_.fdatasync(fd_);
        if (r < 0) return r;
        const uint64_t bytes = sink_->size();
        enc_.reset();
        sink_.reset();
        const int c = ops_.close(fd_);
        fd_ = -1;
        done_frames_ += part_frames_;
        done_bytes_ += bytes;
        part_frames_ = 0;
        return c;
    }
    int r = ops_.fdatasync(fd_);
    if (r < 0) return r;
    HeaderLayout l;
    l.block_align = ba_;
    l.rf64 = rf64_;
    r = apply_patches(size_patches(l, part_data_));
    if (r == 0) r = ops_.fdatasync(fd_);
    int c = ops_.close(fd_);
    fd_ = -1;
    return r < 0 ? r : c;
}

int Writer::do_sync() {
    if (enc_) {
        /* everything the library lets out goes to the file first; then the file is durable up to frames_out */
        int r = enc_->flush();
        if (r == 0) r = ops_.fdatasync(fd_);
        if (r < 0) return r;
        synced_frames_ = done_frames_ + enc_->stats().frames_out;
        if (cfg_.on_sync) cfg_.on_sync(synced_frames_);
        unsynced_ = 0;
        last_sync_ms_ = clk_.mono_ms();
        syncs_++;
        if (!cfg_.recordings_dir.empty()) journal_update("open", ""); /* a failed journal write must not stop the audio */
        return 0;
    }
    int r = ops_.fdatasync(fd_);
    if (r < 0) return r;
    synced_frames_ = total_bytes_ / fb_;
    if (cfg_.on_sync) cfg_.on_sync(synced_frames_);
    HeaderLayout l;
    l.block_align = ba_;
    l.rf64 = rf64_;
    r = apply_patches(size_patches(l, part_data_));
    if (r < 0) return r;
    unsynced_ = 0;
    last_sync_ms_ = clk_.mono_ms();
    syncs_++;
    if (!cfg_.recordings_dir.empty()) journal_update("open", ""); /* a failed journal write must not stop the audio */
    return 0;
}

bool Writer::rotate() {
    int r = finalize_part();
    if (r < 0) {
        fail(-r);
        return false;
    }
    synced_frames_ = total_bytes_ / fb_;
    r = open_part(part_ + 1);
    if (r < 0) {
        fail(-r);
        return false;
    }
    last_sync_ms_ = clk_.mono_ms();
    if (!cfg_.recordings_dir.empty()) journal_update("open", "");
    return true;
}

bool Writer::encode_frames(const unsigned char *p, size_t nframes) {
    while (nframes > 0) {
        size_t can = nframes;
        if (cfg_.auto_split && part_frames_ > 0) {
            /* the part ends before its file could pass split_bytes: every frame the encoder still holds and every frame
             * of this block reach the file at most at their PCM size (plus split_slack for frame and page headers) */
            const uint64_t held = enc_->stats().pending_frames() * fb_;
            const uint64_t used = sink_->size() + held + cfg_.split_slack;
            const uint64_t room = cfg_.split_bytes > used ? cfg_.split_bytes - used : 0;
            can = static_cast<size_t>(std::min<uint64_t>(nframes, room / fb_));
            if (can == 0) {
                if (!rotate()) return false;
                continue;
            }
        }
        const int r = enc_->write(p, can);
        if (r < 0) {
            fail(-r);
            return false;
        }
        const uint64_t bytes = static_cast<uint64_t>(can) * fb_;
        part_frames_ += can;
        part_data_ += bytes;
        total_bytes_ += bytes;
        unsynced_ += bytes;
        p += bytes;
        nframes -= can;
    }
    return true;
}

bool Writer::write_frames(const unsigned char *p, size_t nframes) {
    if (enc_) return encode_frames(p, nframes);
    while (nframes > 0) {
        size_t can = nframes;
        if (cfg_.auto_split) {
            const uint64_t used = kDataOffset + part_data_;
            const uint64_t room = cfg_.split_bytes > used ? cfg_.split_bytes - used : 0;
            can = std::min<size_t>(nframes, static_cast<size_t>(room / fb_));
            if (can == 0) {
                if (part_data_ > 0) {
                    if (!rotate()) return false;
                    continue;
                }
                can = 1; /* a part holds at least one frame */
            }
        } else if (!rf64_ && kDataOffset - 8 + part_data_ + static_cast<uint64_t>(nframes) * fb_ > cfg_.riff_limit) {
            int r = switch_rf64();
            if (r < 0) {
                fail(-r);
                return false;
            }
        }
        size_t bytes = can * fb_;
        const unsigned char *q = p;
        int stalls = 0;
        while (bytes > 0) {
            ssize_t w = ops_.write(fd_, q, bytes);
            if (w < 0) {
                if ((w == -EINTR || w == -EAGAIN) && ++stalls < 100) continue;
                fail(static_cast<int>(-w));
                return false;
            }
            if (w == 0) {
                if (++stalls < 100) continue;
                fail(EIO);
                return false;
            }
            q += w;
            bytes -= static_cast<size_t>(w);
            part_data_ += static_cast<uint64_t>(w);
            total_bytes_ += static_cast<uint64_t>(w);
            unsynced_ += static_cast<uint64_t>(w);
        }
        p += can * fb_;
        nframes -= can;
    }
    return true;
}

bool Writer::drain_step(bool force) {
    const uint64_t consumed = ring_.frames_read();
    uint64_t limit = UINT64_MAX;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        while (!cmds_.empty() && cmds_.front().pos <= consumed) {
            paused_ = cmds_.front().pause;
            if (state_ == WriterState::Recording || state_ == WriterState::Paused)
                state_ = paused_ ? WriterState::Paused : WriterState::Recording;
            cmds_.pop_front();
        }
        if (!cmds_.empty()) limit = cmds_.front().pos - consumed;
    }
    const size_t avail = static_cast<size_t>(std::min<uint64_t>(ring_.available(), limit));
    if (avail == 0) return false;
    const size_t blockf = cfg_.block_bytes / fb_;
    scratch_.resize(blockf * fb_);
    if (paused_) {
        const size_t n = ring_.read(scratch_.data(), std::min(avail, blockf));
        paused_frames_ += n;
        return n > 0;
    }
    const uint64_t now = clk_.mono_ms();
    const bool bound = limit != UINT64_MAX && avail == limit;
    if (avail < blockf && !force && !bound && now - last_write_ms_ < cfg_.flush_ms) return false;
    const size_t n = ring_.read(scratch_.data(), std::min(avail, blockf));
    if (n == 0) return false;
    blocks_++;
    last_write_ms_ = now;
    write_frames(scratch_.data(), n);
    return true;
}

bool Writer::pump(bool *busy) {
    if (busy) *busy = false;
    if (state_ != WriterState::Recording && state_ != WriterState::Paused) return false;
    if (stop_req_.load()) {
        finish(StopReason::User);
        return false;
    }
    const bool did = drain_step(false);
    if (busy) *busy = did;
    if (state_ == WriterState::Failed) return false;
    const uint64_t now = clk_.mono_ms();
    if (now - last_sync_ms_ >= sync_ms_) {
        /* A sync is a durable point: the audio that has arrived by now goes into it, however small the partial block is
         * (at 16000 Hz mono a 128 KB block is 4 s: waiting for a full block would leave seconds outside every sync). */
        size_t guard = ring_.available() / std::max<size_t>(cfg_.block_bytes / fb_, 1) + 2;
        while (guard-- > 0 && state_ != WriterState::Failed && drain_step(true)) {
        }
        if (state_ == WriterState::Failed) return false;
    }
    if (unsynced_ > 0 && now - last_sync_ms_ >= sync_ms_) {
        int r = do_sync();
        if (r < 0) {
            fail(-r);
            return false;
        }
    }
    if (now - last_space_ms_ >= cfg_.space_check_ms) {
        last_space_ms_ = now;
        uint64_t avail = 0;
        if (ops_.free_space(path_, avail) == 0 && avail < cfg_.min_free_bytes) {
            finish(StopReason::LowSpace);
            return false;
        }
    }
    publish();
    return true;
}

void Writer::abandon_file() {
    if (fd_ < 0) return;
    if (enc_) {
        /* no finish(): the sink failed or the library broke. The file may end in a cut frame or page; it is mended in
         * place from what it holds, exactly as the repair at the next launch would (best effort) */
        enc_.reset();
        sink_.reset();
        ops_.fdatasync(fd_);
        ops_.close(fd_);
        fd_ = -1;
        const Container k = cfg_.codec == "flac" ? Container::Flac
                            : cfg_.codec == "opus" ? Container::OggOpus
                                                   : container_of(ops_, path_);
        if (k == Container::Flac) fix_flac(ops_, path_, true);
        else if (k == Container::OggOpus) fix_ogg_opus(ops_, path_, true);
        return;
    }
    HeaderLayout l;
    l.block_align = ba_;
    l.rf64 = rf64_;
    apply_patches(size_patches(l, part_data_)); /* best effort: repair does the same from the file length */
    ops_.fdatasync(fd_);
    ops_.close(fd_);
    fd_ = -1;
}

void Writer::fail(int err) {
    if (state_ == WriterState::Failed || state_ == WriterState::Stopped) return;
    error_ = err;
    reason_ = err == ENOSPC ? StopReason::LowSpace : StopReason::IoError;
    reason_text_ = std::string(err == ENOSPC ? "disk full: " : "write error: ") + error_text(err);
    abandon_file();
    state_ = WriterState::Failed;
    if (!cfg_.recordings_dir.empty()) journal_update("error", reason_text_);
    publish();
}

void Writer::finish(StopReason r) {
    if (finishing_ || (state_ != WriterState::Recording && state_ != WriterState::Paused)) return;
    finishing_ = true;
    while (state_ != WriterState::Failed && drain_step(true)) {
    }
    if (state_ != WriterState::Failed) {
        int rc = finalize_part();
        if (rc < 0) {
            fail(-rc);
        } else {
            synced_frames_ = total_bytes_ / fb_;
            reason_ = r;
            if (r == StopReason::LowSpace) reason_text_ = "free space is low: the take was closed";
            state_ = WriterState::Stopped;
            if (!cfg_.recordings_dir.empty()) journal_update("closed", reason_text_);
            publish();
        }
    }
    finishing_ = false;
}

int Writer::journal_update(const std::string &state, const std::string &reason) {
    je_.state = state;
    je_.reason = reason;
    je_.frames = total_bytes_ / fb_;
    je_.frames_synced = synced_frames_;
    je_.paused_frames = paused_frames_;
    je_.dropped_frames = ring_.frames_dropped();
    if (cfg_.lost_frames) je_.lost_frames = cfg_.lost_frames->load(std::memory_order_relaxed);
    je_.last_sync_wall = clk_.wall_s();
    return journal_write(ops_, cfg_.recordings_dir, je_);
}

void Writer::publish() {
    std::lock_guard<std::mutex> lk(mtx_);
    pub_.state = state_;
    pub_.reason = reason_;
    pub_.reason_text = reason_text_;
    pub_.error = error_;
    pub_.frames_written = total_bytes_ / (fb_ ? fb_ : 1);
    pub_.frames_synced = synced_frames_;
    pub_.bytes_out = done_bytes_ + (sink_ ? sink_->size() : 0);
    pub_.paused_frames = paused_frames_;
    pub_.dropped_frames = ring_.frames_dropped();
    pub_.part = part_;
    pub_.syncs = syncs_;
    pub_.blocks = blocks_;
    pub_.path = path_;
}

void Writer::start_thread() {
    if (thread_.joinable() || state_ != WriterState::Recording) return;
    thread_ = std::thread([this] {
        bool busy = false;
        while (pump(&busy))
            if (!busy) clk_.sleep_ms(5);
    });
}

void Writer::stop() {
    if (thread_.joinable()) {
        stop_req_.store(true);
        thread_.join();
    } else {
        finish(StopReason::User);
    }
}

void Writer::pause() {
    std::lock_guard<std::mutex> lk(mtx_);
    cmds_.push_back(Cmd{ring_.frames_written(), true});
}

void Writer::resume() {
    std::lock_guard<std::mutex> lk(mtx_);
    cmds_.push_back(Cmd{ring_.frames_written(), false});
}

WriterStatus Writer::status() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return pub_;
}

}  // namespace cassette
