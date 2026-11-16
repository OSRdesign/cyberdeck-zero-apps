/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: a camcorder and photo app for USB cameras (plan 062). The headless commands (--bench, --list, --selftest) run
 * without the display; otherwise the device runner (_common/platform.hpp) shows the app (ui/vhs_app.hpp).
 */
#include "live/bench.hpp"
#include "platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    vhs::Cli cli;
    std::string err;
    if (!vhs::parse_cli(args, cli, err)) {
        std::fprintf(stderr, "vhs: %s\n%s", err.c_str(), vhs::cli_usage().c_str());
        return 2;
    }
    switch (cli.cmd) {
    case vhs::Cli::Cmd::Help: std::fputs(vhs::cli_usage().c_str(), stdout); return 0;
    case vhs::Cli::Cmd::Bench: return vhs::run_bench(cli);
    case vhs::Cli::Cmd::List: return vhs::run_list(cli);
    case vhs::Cli::Cmd::Selftest: return vhs::run_selftest();
    case vhs::Cli::Cmd::Ui: break;
    }
    for (const auto &kv : cli.env) setenv(kv.first.c_str(), kv.second.c_str(), 1);
    std::vector<char *> pass{argv[0]};
    for (const std::string &p : cli.pass) pass.push_back(const_cast<char *>(p.c_str()));
    pass.push_back(nullptr);
    return cdk::run(cdk::app_spec(), static_cast<int>(pass.size()) - 1, pass.data());
}
