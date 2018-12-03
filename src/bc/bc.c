/*
 * *****************************************************************************
 *
 * Copyright 2018 Gavin D. Howard
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 *
 * *****************************************************************************
 *
 * The main procedure of bc.
 *
 */

#include <status.h>
#include <bc.h>
#include <vm.h>

#if BC_ENABLED
int bc_main(int argc, char *argv[]) {

	bcg.name = bc_name;
	bcg.help = bc_help;
#if BC_ENABLE_SIGNALS
	bcg.sig_msg = bc_sig_msg;
#endif // BC_ENABLE_SIGNALS

	bcg.init = bc_parse_init;
	bcg.exp = bc_parse_expression;
	bcg.sbgn = bcg.send = '"';

	return (int) bc_vm_run(argc, argv, "BC_LINE_LENGTH");
}
#endif // BC_ENABLED
