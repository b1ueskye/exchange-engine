#ifndef EXCHANGE_TEST_CHECKS_HPP
#define EXCHANGE_TEST_CHECKS_HPP

#include <iostream>

class TestChecks {
  public:
    void require(bool condition, const char *label) {
        if (!condition) {
            std::cerr << "FAIL: " << label << '\n';
            ++failures_;
        }
    }

    int exit_code() const {
        if (failures_ == 0) {
            return 0;
        }
        return 1;
    }

  private:
    int failures_{};
};

#endif