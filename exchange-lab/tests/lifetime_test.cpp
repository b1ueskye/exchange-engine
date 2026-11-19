#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

enum class Exit { normal, early, error };

class Trace {
public:
    Trace(int tag, int (&steps)[2], int& used) : tag_{tag}, steps_{steps}, used_{used} {}
    Trace(const Trace&) = delete;
    Trace& operator=(const Trace&) = delete;

    ~Trace() {
        steps_[used_++] = tag_;
    }

private:
    int tag_;
    int (&steps_)[2];
    int& used_;
};

void leave(Exit mode, int (&steps)[2], int& used) {
    const Trace outer{1, steps, used};
    const Trace inner{2, steps, used};
    if (mode == Exit::early) {
        return;
    }
    if (mode == Exit::error) {
        throw std::runtime_error{"simulated failure"};
    }
}

bool write_lots(const std::filesystem::path& path, int lots) {
    std::ofstream file{path};
    if (!file) {
        return false;
    }
    file << lots << '\n';
    file.close();
    return static_cast<bool>(file);
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "expected a build directory test path\n";
        return 1;
    }

    const Exit modes[]{Exit::normal, Exit::early, Exit::error};
    for (const auto mode : modes) {
        int steps[2]{};
        int used = 0;
        bool caught = false;
        try {
            leave(mode, steps, used);
        } catch (const std::runtime_error&) {
            caught = true;
        }
        if (used != 2 || steps[0] != 2 || steps[1] != 1 ||
            caught != (mode == Exit::error)) {
            std::cerr << "FAIL: exit or destruction order\n";
            return 1;
        }
    }

    auto owner = std::make_unique<int>(3);
    const int* view = owner.get();
    auto next_owner = std::move(owner);
    if (owner || !next_owner || next_owner.get() != view || *view != 3) {
        std::cerr << "FAIL: ownership transfer\n";
        return 1;
    }

    const std::filesystem::path path{argv[1]};
    if (!write_lots(path, *next_owner)) {
        std::cerr << "FAIL: writing the test file\n";
        return 1;
    }

    int saved = 0;
    {
        std::ifstream file{path};
        if (!(file >> saved) || saved != 3) {
            std::cerr << "FAIL: reading the closed file\n";
            return 1;
        }
    }
    if (write_lots(path / "child", 4)) {
        std::cerr << "FAIL: opened a child of a regular file\n";
        return 1;
    }

    std::filesystem::remove(path);
    std::cout << "destruction: 2 then 1; file: " << saved << '\n';
    return 0;
}