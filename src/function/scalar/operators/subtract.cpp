#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/operator/add.hpp"

#include "duckdb/common/limits.hpp"
#include "duckdb/common/types/value.hpp"

#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/hugeint.hpp"

#include <limits>

using namespace std;

namespace duckdb {

//===--------------------------------------------------------------------===//
// - [subtract]
//===--------------------------------------------------------------------===//
template <> float SubtractOperator::Operation(float left, float right) {
	auto result = left - right;
	if (!Value::FloatIsValid(result)) {
		throw OutOfRangeException("Overflow in subtraction of float!");
	}
	return result;
}

template <> double SubtractOperator::Operation(double left, double right) {
	auto result = left - right;
	if (!Value::DoubleIsValid(result)) {
		throw OutOfRangeException("Overflow in subtraction of double!");
	}
	return result;
}

template <> interval_t SubtractOperator::Operation(interval_t left, interval_t right) {
	interval_t result;
	result.months = left.months - right.months;
	result.days = left.days - right.days;
	result.msecs = left.msecs - right.msecs;
	return result;
}

template <> date_t SubtractOperator::Operation(date_t left, interval_t right) {
	right.months = -right.months;
	right.days = -right.days;
	right.msecs = -right.msecs;
	return AddOperator::Operation<date_t, interval_t, date_t>(left, right);
}

template <> timestamp_t SubtractOperator::Operation(timestamp_t left, interval_t right) {
	right.months = -right.months;
	right.days = -right.days;
	right.msecs = -right.msecs;
	return AddOperator::Operation<timestamp_t, interval_t, timestamp_t>(left, right);
}

template <> interval_t SubtractOperator::Operation(timestamp_t left, timestamp_t right) {
	return Interval::GetDifference(left, right);
}

//===--------------------------------------------------------------------===//
// - [subtract] with overflow check
//===--------------------------------------------------------------------===//
struct OverflowCheckedSubtract {
	template<class SRCTYPE, class UTYPE>
	static inline bool Operation(SRCTYPE left, SRCTYPE right, SRCTYPE &result) {
		UTYPE uresult = SubtractOperator::Operation<UTYPE, UTYPE, UTYPE>(UTYPE(left), UTYPE(right));
		if (uresult < NumericLimits<SRCTYPE>::Minimum() || uresult > NumericLimits<SRCTYPE>::Maximum()) {
			return false;
		}
		result = SRCTYPE(uresult);
		return true;
	}
};

template <> bool TrySubtractOperator::Operation(int8_t left, int8_t right, int8_t &result) {
	return OverflowCheckedSubtract::Operation<int8_t, int16_t>(left, right, result);
}

template <> bool TrySubtractOperator::Operation(int16_t left, int16_t right, int16_t &result) {
	return OverflowCheckedSubtract::Operation<int16_t, int32_t>(left, right, result);
}

template <> bool TrySubtractOperator::Operation(int32_t left, int32_t right, int32_t &result) {
	return OverflowCheckedSubtract::Operation<int32_t, int64_t>(left, right, result);
}

template <> bool TrySubtractOperator::Operation(int64_t left, int64_t right, int64_t &result) {
#if defined(__GNUC__) || defined(__clang__)
	if (__builtin_sub_overflow(left, right, &result)) {
		return false;
	}
	// FIXME: this check can be removed if we get rid of NullValue<T>
	if (result == std::numeric_limits<int64_t>::min()) {
		return false;
	}
#else
	if (right < 0) {
		if (NumericLimits<int64_t>::Maximum() + right < left) {
			return false;
		}
	} else {
		if (NumericLimits<int64_t>::Minimum() + right > left) {
			return false;
		}
	}
	result = left - right;
#endif
	return true;
}

//===--------------------------------------------------------------------===//
// subtract decimal with overflow check
//===--------------------------------------------------------------------===//
template <> bool TryDecimalSubtract::Operation(int64_t left, int64_t right, int64_t& result) {
	if (right < 0) {
		if (999999999999999999 + right < left) {
			return false;
		}
	} else {
		if (-999999999999999999 + right > left) {
			return false;
		}
	}
	result = left - right;
	return true;
}

template <> bool TryDecimalSubtract::Operation(hugeint_t left, hugeint_t right, hugeint_t& result) {
	result = left - right;
	if (result <= -Hugeint::PowersOfTen[38] || result >= Hugeint::PowersOfTen[38]) {
		return false;
	}
	return true;
}

template <> hugeint_t DecimalSubtractOverflowCheck::Operation(hugeint_t left, hugeint_t right) {
	hugeint_t result;
	if (!TryDecimalSubtract::Operation(left, right, result)) {
		throw OutOfRangeException("Overflow in subtract of DECIMAL(38) (%s - %s);", left.ToString(), right.ToString());
	}
	return result;
}

//===--------------------------------------------------------------------===//
// subtract time operator
//===--------------------------------------------------------------------===//
template <> dtime_t SubtractTimeOperator::Operation(dtime_t left, interval_t right) {
	right.msecs = -right.msecs;
	return AddTimeOperator::Operation<dtime_t, interval_t, dtime_t>(left, right);
}

}
