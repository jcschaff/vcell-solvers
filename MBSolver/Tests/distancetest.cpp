#include "gtest/gtest.h"
#include <Distance.h>
using std::cout;
using std::endl;
using namespace spatial;

TEST(distance,basic) {
	int64_t biggest = std::numeric_limits<int64_t>::max( );
	int64_t okay = DefaultDistancePolicy<int64_t>::convert(biggest);
	ASSERT_TRUE(okay == biggest);
    // jcs: switched test to long double, a double cannot hold int64_t max+1 without underflow, using long double
	long double tooBig = static_cast<long double>(biggest) + 1;
	ASSERT_THROW(DefaultDistancePolicy<int64_t>::convert(tooBig),std::domain_error);
	cout << okay << endl;
}
