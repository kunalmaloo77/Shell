#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

#ifdef _WIN32
#include <io.h> // for _access
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h> // for access()

#endif

#ifdef _WIN32
constexpr char PATH_LIST_SEPARATOR = ';';
#else
constexpr char PATH_LIST_SEPARATOR = ':';
#endif

bool isExecutableFile(const std::string &path) {
#ifdef _WIN32
  fs::path p(path);
  std::string ext = p.extension().string();
  return (ext == ".exe" || ext == ".com" || ext == ".bat" || ext == ".cmd") &&
         fs::exists(p);
#else
  // string::data and string::c_str are synonyms and return same value
  return fs::exists(path) && (access(path.c_str(), X_OK) == 0);
#endif
}

void executeProgram(const std::string &path,
                    const std::vector<std::string> &args) {
#ifdef _WIN32
  std::string cmdLine;
  for (const auto &arg : args) {
    cmdLine += "\"" + arg + "\" ";
  }

  STARTUPINFOA si{};
  PROCESS_INFORMATION pi{};
  si.cb = sizeof(si);

  if (!CreateProcessA(path.c_str(), cmdLine.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, nullptr, &si, &pi)) {
    std::cerr << "CreateProcess failed\n";
    return;
  }

  WaitForSingleObject(pi.hProcess, INFINITE);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
#else
  pid_t pid = fork();
  if (pid == 0) {
    std::vector<char *> argv;
    argv.reserve(args.size() + 1); // optimize allocation
    for (auto &s : args)
      argv.push_back(const_cast<char *>(s.c_str()));
    argv.push_back(nullptr);

    execv(path.c_str(), argv.data());
    perror("execv");
    _exit(1);
  } else {
    waitpid(pid, nullptr, 0);
  }
#endif
}

std::string trim(const std::string &str) {
  size_t start = str.find_first_not_of(" \t");
  if (start == std::string::npos)
    return "";

  size_t end = str.find_last_not_of(" \t");
  return str.substr(start, end - start + 1);
}

std::vector<std::string> tokenize(const std::string &input) {
  std::vector<std::string> tokens;
  std::string current_token;
  bool inside_quotes = false;
  for (char c : input) {
    if (c == '\'') {
      inside_quotes = !inside_quotes;
    } else if (c == ' ' || c == '\t') {
      if (inside_quotes) {
        current_token += c; // treat space as literal inside quote
      } else {
        if (!current_token.empty()) {
          tokens.push_back(current_token);
          current_token.clear();
        }
      }
    } else {
      current_token += c;
    }
  }
  if (!current_token.empty()) {
    tokens.push_back(current_token);
  }
  return tokens;
}

std::vector<std::string> tokenizePath(const std::string &pathEnv,
                                      char separator) {
  std::istringstream iss(pathEnv);
  std::vector<std::string> dirs;
  std::string dir;

  while (std::getline(iss, dir, separator)) {
    if (dir.empty())
      dir = "."; // empty entry = current directory
    dirs.push_back(dir);
  }
  return dirs;
}

std::string getExecutablePath(const std::string &command) {
  const char *env_p = getenv("PATH");
  if (!env_p)
    return "";
  std::string path_env(env_p);
  std::vector<std::string> dirs = tokenizePath(path_env, PATH_LIST_SEPARATOR);
#ifdef _WIN32
  std::vector<std::string> exts = {".exe", ".bat", ".cmd", ".com"};
#else
  std::vector<std::string> exts = {""};
#endif
  for (auto &&dir : dirs) {
    for (auto &&ext : exts) {
      fs::path full =
          fs::path(dir) / (command + ext); // '/' acts as operator overload
      if (isExecutableFile(full.string())) {
        return full.string();
      }
    }
  }
  return "";
}

const std::unordered_set<std::string> builtins = {"type", "exit", "echo", "pwd",
                                                  "cd"};

int main() {
  // Flush after every std::cout / std:cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  while (true) {
    std::cout << "$ ";
    std::string command;

    if (!std::getline(std::cin, command))
      break;

    command = trim(command);

    if (command.find_first_not_of(" \t") == std::string::npos)
      continue;

    std::vector<std::string> tokens = tokenize(command);
    if (tokens[0] == "exit")
      break;
    else if (tokens[0] == "echo") {
      for (size_t i = 1; i < tokens.size(); ++i) {
        std::cout << tokens[i];
        if (i + 1 < tokens.size())
          std::cout << " ";
      }
      std::cout << std::endl;
    } else if (tokens[0] == "type") {
      for (size_t i = 1; i < tokens.size(); ++i) {
        const std::string &cmd = tokens[i];
        if (builtins.find(cmd) != builtins.end()) {
          std::cout << cmd << " is a shell builtin" << std::endl;
        } else {
          std::string path = getExecutablePath(cmd);
          if (!path.empty()) {
            std::cout << cmd << " is " << path << std::endl;
          } else {
            std::cout << cmd << ": not found" << std::endl;
          }
        }
      }
    } else if (tokens[0] == "pwd") {
      std::cout << fs::current_path().string() << std::endl;
    } else if (tokens[0] == "cd") {
      if (tokens.size() == 1)
        fs::current_path("/");
      else {
        std::vector<std::string> parts(tokens.begin() + 1, tokens.end());
        fs::path p;
        if (parts.size() == 1 && parts[0] == "~") {
          const char *home_env = getenv("HOME");
          if (!home_env)
            std::cout << "cd: HOME env not found";

          std::string string_env(home_env);
          fs::path home_path(string_env);
          try {
            fs::current_path(home_path);
          } catch (const fs::filesystem_error &e) {
            std::cout << "cd: " << e.what() << '\n';
          }
        } else {
          for (const auto &part : parts) {
            p /= part;
          }
          if (fs::exists(p))
            fs::current_path(p);
          else {
            std::cout << "cd: " << p.string()
                      << ": No such file or directory\n";
          }
        }
      }
    } else {
      std::string path = getExecutablePath(tokens[0]);
      if (path.empty()) {
        std::cout << tokens[0] << ": command not found" << std::endl;
        continue;
      }
      executeProgram(path, tokens);
    }
  }
}
