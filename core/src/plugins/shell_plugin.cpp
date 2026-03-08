#include <anyar/plugins/shell_plugin.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace anyar {

// ── Helpers ─────────────────────────────────────────────────────────────────

/// Shell-escape a single argument.
static std::string shell_escape(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

/// Run a command via popen and capture stdout+stderr+exit code.
struct ExecResult {
    int code;
    std::string out;  // stdout
    std::string err;  // stderr
};

static ExecResult run_command(const std::string& program,
                              const std::vector<std::string>& args,
                              const std::string& cwd) {
    // Build command string
    std::string cmd = shell_escape(program);
    for (auto& a : args) {
        cmd += " " + shell_escape(a);
    }
    // Redirect stderr to a temp pipe by using 2>&1 trick:
    //   ( cmd ) 2>/tmp/anyar_stderr_$$ ; echo $? ; cat /tmp/anyar_stderr_$$
    // Simpler approach: capture combined, or use fork/exec.
    // We'll use a dual-pipe approach via fork.

    int stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        throw std::runtime_error("Failed to create pipes");
    }

    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork() failed");
    }

    if (pid == 0) {
        // Child process
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        if (!cwd.empty()) {
            if (chdir(cwd.c_str()) != 0) {
                _exit(127);
            }
        }

        // Build argv
        std::vector<const char*> argv;
        argv.push_back(program.c_str());
        for (auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);

        execvp(program.c_str(), const_cast<char* const*>(argv.data()));
        // If exec fails
        _exit(127);
    }

    // Parent process
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    auto read_all = [](int fd) -> std::string {
        std::string result;
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            result.append(buf, static_cast<size_t>(n));
        }
        return result;
    };

    std::string stdout_str = read_all(stdout_pipe[0]);
    std::string stderr_str = read_all(stderr_pipe[0]);
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    int status;
    waitpid(pid, &status, 0);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    return {exit_code, std::move(stdout_str), std::move(stderr_str)};
}

// ── Plugin registration ─────────────────────────────────────────────────────

void ShellPlugin::initialize(PluginContext& ctx) {
    auto& cmds = ctx.commands;

    // ── shell:openUrl ───────────────────────────────────────────────────────
    cmds.add("shell:openUrl", [](const json& args) -> json {
        std::string url = args.at("url").get<std::string>();
        // xdg-open on Linux
        std::string cmd = "xdg-open " + shell_escape(url) + " &";
        int ret = std::system(cmd.c_str());
        if (ret != 0) {
            throw std::runtime_error("Failed to open URL: " + url);
        }
        return nullptr;
    });

    // ── shell:openPath ──────────────────────────────────────────────────────
    cmds.add("shell:openPath", [](const json& args) -> json {
        std::string path = args.at("path").get<std::string>();
        std::string cmd = "xdg-open " + shell_escape(path) + " &";
        int ret = std::system(cmd.c_str());
        if (ret != 0) {
            throw std::runtime_error("Failed to open path: " + path);
        }
        return nullptr;
    });

    // ── shell:execute ───────────────────────────────────────────────────────
    cmds.add("shell:execute", [](const json& args) -> json {
        std::string program = args.at("program").get<std::string>();

        std::vector<std::string> cmd_args;
        if (args.contains("args") && args["args"].is_array()) {
            for (auto& a : args["args"]) {
                cmd_args.push_back(a.get<std::string>());
            }
        }

        std::string cwd = args.value("cwd", "");

        auto result = run_command(program, cmd_args, cwd);

        return {
            {"code", result.code},
            {"stdout", result.out},
            {"stderr", result.err},
        };
    });
}

} // namespace anyar
