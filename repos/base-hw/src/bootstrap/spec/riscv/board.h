/*
 * \brief   Riscv spike specific board definitions
 * \author  Stefan Kalkowski
 * \date    2017-02-20
 */

/*
 * Copyright (C) 2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

#ifndef _SRC__BOOTSTRAP__SPEC__RISCV__BOARD_H_
#define _SRC__BOOTSTRAP__SPEC__RISCV__BOARD_H_

#include <hw/spec/riscv/page_table.h>
#include <hw/spec/riscv/uart.h>

namespace Genode { struct Board_base {}; }

namespace Bootstrap {
	struct Cpu {};
	struct Pic {};

	enum { UART_BASE, UART_CLOCK };
	struct Serial : Hw::Riscv_uart {
		Serial(unsigned, unsigned, unsigned) {} };
}

template <typename E, unsigned B, unsigned S>
void Sv39::Level_x_translation_table<E, B, S>::_translation_added(addr_t addr,
                                                                  size_t size)
{ }

#endif /* _SRC__BOOTSTRAP__SPEC__RISCV__BOARD_H_ */