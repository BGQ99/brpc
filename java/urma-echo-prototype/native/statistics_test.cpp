#include "statistics.h"
#include <iostream>
#include <stdexcept>

int main() {
    echo_proto::Histogram histogram;
    if (histogram.Percentile(99) != -1) throw std::runtime_error("empty histogram must be unavailable");
    for (uint64_t i = 1; i <= 10000; ++i) histogram.Add(i);
    for (unsigned percent : {50U, 95U, 99U}) {
        const int64_t exact = percent * 100;
        const int64_t estimate = histogram.Percentile(percent);
        if (estimate < exact || estimate > exact * 1.016 + 1) {
            throw std::runtime_error("percentile upper bound outside documented precision");
        }
    }
    std::cout << "HISTOGRAM_TEST=PASS" << std::endl;
}
