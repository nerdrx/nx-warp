#include "nxastc_backup_policy.h"

#include <cassert>

using namespace wivrn::nxastc_backup;

int main()
{
	policy p;
	const pair asymmetric{0, 0, 9000, 9000, 100, 89};
	assert(p.select(100, 10, asymmetric, {}) == choice::hold); // older eye controls age
	const pair primary{0, 0, 9000, 9000, 100, 100};
	assert(p.select(100, 10, primary, {}) == choice::primary); // frame zero is valid

	const pair backup{1, 1, 10000, 10000, 115, 116}; // future target is not a capture-age measurement
	assert(p.select(119, 10, primary, backup) == choice::hold);
	assert(p.select(120, 10, primary, backup) == choice::backup); // 2 periods since primary selected
	assert(p.select(121, 10, primary, pair{2, 2, 11000, 11000, 100, 121}) == choice::hold); // stale left decode
	assert(p.select(122, 10, pair{3, 3, 11000, 11000, 122, 122}, backup) == choice::primary); // late fresh primary
	assert(p.select(123, 10, {}, pair{4, 4, 10999, 10999, 123, 123}) == choice::hold); // target order cannot rewind
	assert(p.select(122, 10, pair{0, 0, 0, 0, 122, 122}, {}) == choice::hold); // clock rollback resets
	assert(p.select(124, 10, pair{0, 0, 0, 0, 124, 124}, {}) == choice::primary);

	policy variable_period;
	assert(variable_period.select(200, 10, pair{5, 5, 1000, 1000, 200, 200}, {}) == choice::primary);
	const pair next_backup{6, 6, 1100, 1100, 219, 219};
	assert(variable_period.select(220, 11, {}, next_backup) == choice::hold);
	assert(variable_period.select(221, 11, {}, next_backup) == choice::hold);
	assert(variable_period.select(222, 10, {}, next_backup) == choice::backup);
	assert(variable_period.select(223, 10, {}, next_backup, false) == choice::hold);
	assert(variable_period.select(224, 10, pair{0, 0, 0, 0, 224, 224}, {}) == choice::primary);
	variable_period.reset();
	assert(variable_period.select(300, 10, {}, next_backup) == choice::hold);
}
