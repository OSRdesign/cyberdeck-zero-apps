# Reticulum License
#
# Copyright (c) 2026 Mark Qvist
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# - The Software shall not be used in any kind of system which includes amongst
#   its functions the ability to purposefully do harm to human beings.
#
# - The Software shall not be used, directly or indirectly, in the creation of
#   an artificial intelligence, machine learning or language model training
#   dataset, including but not limited to any use that contributes to the
#   training or development of such a model or algorithm.
#
# - The above copyright notice and this permission notice shall be included in
#   all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

from collections import deque
import threading

# Zero-copy coalesced transmit buffer for arbitrary
# frame sizes. It's even almost 100% lock-free.
class TransmitBuffer:
    COALESCE_TARGET = 65536

    def __init__(self):
        self._tail_lock  = threading.Lock()
        
        # Producer-owned state
        self._cur        = None
        self._cur_frames = 0

        # Consumer-owned state
        self._head_offset = 0

        # Shared chunk queue. Producer appends right, consumer
        # pops from left. Appended chunks are immutable.
        self._chunks = deque()

        # Accounting counters

        # Producer-owned
        self._tx_total       = 0  # Total bytes appended
        self._tx_visible     = 0  # Total bytes pushed to the chunk queue
        self._tx_frames      = 0  # Total frames appended
        
        # Consumer-owned
        self._tx_sent        = 0  # Total bytes sent to the kernel
        self._tx_frames_sent = 0  # Total frames fully sent

    # Queue a finalized and complete on-wire frame.
    # Executed by producer side only.
    def append(self, frame, limit=None):
        with self._tail_lock:
            if limit is not None and (self._tx_total - self._tx_sent) + len(frame) > limit: return False

            # Allocate separate chunks to frames
            # larger than the coalescing target.
            if len(frame) >= self.COALESCE_TARGET:
                self._flush()
                self._chunks.append((frame, 1))
                self._tx_visible += len(frame)
                self._tx_total   += len(frame)
                self._tx_frames  += 1

            else:
                if self._cur is not None and len(self._cur) + len(frame) > self.COALESCE_TARGET: self._flush()

                if self._cur is None:
                    self._cur        = bytearray()
                    self._cur_frames = 0

                self._cur.extend(frame)
                self._cur_frames += 1
                self._tx_total   += len(frame)
                self._tx_frames  += 1

                if not self._chunks:
                    # Queue is idle. Make the frame visible immediately so
                    # sparse traffic does not incur coalescing latency.
                    self._flush()

        return True

    def _flush(self):
        if self._cur is not None and len(self._cur) > 0:
            self._chunks.append((self._cur, self._cur_frames))
            self._tx_visible += len(self._cur)
            self._cur        = None
            self._cur_frames = 0

    # Make producer-side coalescing tail visible to the consumer.
    # Called by producer when it has finished producing a burst,
    # and wants to make the remaining bytes transmittable. Safe to
    # call after every append.
    def flush(self):
        with self._tail_lock: self._flush()

    # Drain buffered frames to a socket. Called by consumer. If the
    # chunk queue is drained completely, any potential producer-side
    # coalescing tail is released under the tail lock, and drained in
    # the same cycle. If you possess wizardry for doing this without
    # the lock, I am all ears.
    def drain_to(self, socket):
        # TODO: At *some* point (no, not now), replace with a
        # mechanism for send_msg to immediately drain all
        # chunks directly to the kernel.
        total = 0
        whole = True
        while whole:
            if not self._chunks:
                with self._tail_lock: self._flush()
                if not self._chunks:  break

            chunk, _ = self._chunks[0]
            remaining = len(chunk) - self._head_offset
            if remaining <= 0:
                self._pop_head()
                continue

            try: written = socket.send(memoryview(chunk)[self._head_offset:])
            except BlockingIOError:  break
            except InterruptedError: continue
            if written <= 0:         break

            total += written
            self._head_offset += written
            self._tx_sent     += written

            if self._head_offset >= len(chunk): self._pop_head()
            else:                               whole = False

        return total

    def _pop_head(self):
        chunk, frames = self._chunks.popleft()
        self._tx_frames_sent += frames
        self._head_offset     = 0

    # Total number of bytes currently buffered, including the
    # producer's in-progress coalescing chunk.
    def __len__(self): return self._tx_total-self._tx_sent

    # The number of bytes actually ready to send, and will be
    # visible to the consumer, that is, what can be drained.
    @property
    def sendable(self): return self._tx_visible-self._tx_sent

    # The number of complete frames currently buffere, including
    # ones contained in a potential in-progress coalescing chunk.
    @property
    def frames_buffered(self): return self._tx_frames-self._tx_frames_sent

    # Number of chunks currently queued and visible to the consumer
    @property
    def chunks_buffered(self): return len(self._chunks)

    def __repr__(self): return f"<TransmitBuffer buffered={len(self)} sendable={self.sendable} frames={self.frames_buffered} chunks={self.chunks_buffered}>"