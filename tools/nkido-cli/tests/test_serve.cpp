// Integration tests for nkido-cli serve mode. We spawn the binary as a
// subprocess, pipe a sequence of JSON commands into stdin, and check stdout
// for the expected events.
//
// The motivating bug: the VS Code "Nkido extension" launches `nkido-cli serve`
// with no `--bank` flag, and previously the JSON wire protocol had no way to
// register a bank. Sample patches typed in the editor would print
// `warning: sample 'X' not found in any loaded bank` and play silence.
// The `load_bank` command added in serve_mode.cpp closes that hole, and this
// test prevents it from regressing.
//
// We run with `SDL_AUDIODRIVER=dummy` so audio init succeeds on headless CI;
// the dummy driver opens a no-op output device.

#include <catch2/catch_test_macros.hpp>

#include "test_paths.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>  // for close()

namespace {

// Pipe `commands` into `argv` over stdin and capture stdout+stderr. Returns
// the captured combined output. exit_status is set to the child's exit code.
std::string run_with_stdin(const std::string& argv,
                           const std::string& commands,
                           int* exit_status) {
    // Write commands to a tempfile, redirect stdin from it, redirect both
    // stdout and stderr to a tempfile we read back. This is portable enough
    // for a Linux dev box without pulling in a process-spawning lib.
    char cmd_path[] = "/tmp/nkido_serve_test_in_XXXXXX";
    int cmd_fd = mkstemp(cmd_path);
    REQUIRE(cmd_fd >= 0);
    {
        std::ofstream cmd_file(cmd_path);
        cmd_file << commands;
    }
    close(cmd_fd);

    char out_path[] = "/tmp/nkido_serve_test_out_XXXXXX";
    int out_fd = mkstemp(out_path);
    REQUIRE(out_fd >= 0);
    close(out_fd);

    std::string shell_cmd =
        "SDL_AUDIODRIVER=dummy " + argv +
        " < " + cmd_path +
        " > " + out_path + " 2>&1";

    const int rc = std::system(shell_cmd.c_str());
    if (exit_status) *exit_status = rc;

    std::ifstream out_file(out_path);
    std::stringstream buf;
    buf << out_file.rdbuf();

    std::remove(cmd_path);
    std::remove(out_path);

    return buf.str();
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("serve mode: load_bank registers a manifest and emits bank_registered",
          "[serve][sample][load_bank]") {
    // Without load_bank, sample patches fail to resolve. With it, the bank is
    // registered immediately and the manifest is fetched once on registration
    // (so the user gets synchronous success/failure feedback, and subsequent
    // load commands don't trigger redundant fetches in the log).
    std::string args = std::string(nkido::test::CLI_BINARY) +
                       " serve --rate 48000 --buffer 512";
    std::string commands;
    commands += R"({"cmd":"load_bank","uri":"file://)" +
                std::string(nkido::test::LOCAL_BANK_MANIFEST) + R"("})" "\n";
    commands += R"({"cmd":"quit"})" "\n";

    int exit_code = -1;
    const std::string output = run_with_stdin(args, commands, &exit_code);

    INFO("stdout/stderr was:\n" << output);
    CHECK(contains(output, R"("event":"ready")"));
    CHECK(contains(output, R"("event":"bank_registered")"));
    CHECK(contains(output, R"("sample_count":1)"));  // local_bank has 1 sample (bd)
    // No load was issued, so we should not see the missing-sample warning.
    CHECK_FALSE(contains(output, "not found in any loaded bank"));
}

TEST_CASE("serve mode: load after load_bank resolves sample names without warnings",
          "[serve][sample][load_bank]") {
    // The user's actual scenario (paraphrased): launch serve, register a bank,
    // then send a sample-pattern source. Before the fix, no bank was registered
    // and the load printed "sample 'bd' not found in any loaded bank". After
    // the fix, load_bank populates s.opts.bank_uris so the subsequent load's
    // call into prepare_program_assets() picks the bank up and resolves
    // "bd" to a valid sample ID.
    std::string args = std::string(nkido::test::CLI_BINARY) +
                       " serve --rate 48000 --buffer 512";
    std::string commands;
    commands += R"({"cmd":"load_bank","uri":"file://)" +
                std::string(nkido::test::LOCAL_BANK_MANIFEST) + R"("})" "\n";
    commands += R"json({"cmd":"load","source":"s\"bd\".out()"})json" "\n";
    commands += R"({"cmd":"stop"})" "\n";
    commands += R"({"cmd":"quit"})" "\n";

    int exit_code = -1;
    const std::string output = run_with_stdin(args, commands, &exit_code);

    INFO("stdout/stderr was:\n" << output);
    CHECK(contains(output, R"("event":"ready")"));
    CHECK(contains(output, R"("event":"bank_registered")"));
    CHECK(contains(output, R"("event":"compiled","ok":true)"));
    CHECK_FALSE(contains(output, "sample 'bd' not found in any loaded bank"));
}

TEST_CASE("serve mode: load without load_bank still warns about missing samples",
          "[serve][sample][load_bank]") {
    // Negative-control test: confirms the warning-on-missing-sample path is
    // still active. If this stops emitting the warning, either we accidentally
    // auto-loaded a default bank (changing the contract) or the diagnostic
    // path is broken — both worth catching.
    std::string args = std::string(nkido::test::CLI_BINARY) +
                       " serve --rate 48000 --buffer 512";
    std::string commands;
    commands += R"json({"cmd":"load","source":"s\"bd\".out()"})json" "\n";
    commands += R"({"cmd":"quit"})" "\n";

    int exit_code = -1;
    const std::string output = run_with_stdin(args, commands, &exit_code);

    INFO("stdout/stderr was:\n" << output);
    CHECK(contains(output, "sample 'bd' not found in any loaded bank"));
}

TEST_CASE("serve mode: load_bank with a bad URI emits an error event",
          "[serve][sample][load_bank]") {
    // Invalid URIs should fail at registration time (immediate fetch) rather
    // than silently succeeding and only erroring on the next load command.
    std::string args = std::string(nkido::test::CLI_BINARY) +
                       " serve --rate 48000 --buffer 512";
    std::string commands;
    commands += R"({"cmd":"load_bank","uri":"file:///definitely/does/not/exist.json"})" "\n";
    commands += R"({"cmd":"quit"})" "\n";

    int exit_code = -1;
    const std::string output = run_with_stdin(args, commands, &exit_code);

    INFO("stdout/stderr was:\n" << output);
    CHECK(contains(output, R"("event":"error")"));
    CHECK(contains(output, "failed to load"));
    // bank_registered must NOT fire for a failed bank.
    CHECK_FALSE(contains(output, R"("event":"bank_registered")"));
}
