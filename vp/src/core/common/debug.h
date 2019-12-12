#ifndef RISCV_DEBUG
#define RISCV_DEBUG

#include <stdint.h>

#include <unordered_set>
#include <vector>

#include "core_defs.h"

/* TODO: For now the debugable class can only be used with RV64, it is
 * howver intended as an abstract interface that should work with both
 * RV32 and RV64.
 *
 * Unfourtunatly, supporting both would require significant changes to
 * iss.cpp, e.g. make sure they return a uniform type for register
 * values.
 */
struct debugable {
	uint64_t pc = 0;
	bool debug_mode = false;
	bool ignore_wfi = false;
	CoreExecStatus status = CoreExecStatus::Runnable;
	std::unordered_set<uint64_t> breakpoints;

	virtual ~debugable() {}

	virtual Architecture get_architecture(void) = 0;

	virtual uint64_t get_hart_id(void) = 0;

	virtual std::vector<uint64_t> get_registers(void) = 0;
	virtual uint64_t read_register(unsigned) = 0;

	virtual void run(void) = 0;
};

#endif
