/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *  
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *  
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/yosys.h"
#include "kernel/satgen.h"
#include "kernel/sigtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct EquivInductWorker
{
	Module *module;
	SigMap sigmap;

	vector<Cell*> cells;
	pool<Cell*> workset;

	ezDefaultSAT ez;
	SatGen satgen;

	int max_seq;
	int success_counter;

	dict<int, int> ez_step_is_consistent;
	pool<Cell*> cell_warn_cache;

	EquivInductWorker(Module *module, const pool<Cell*> &unproven_equiv_cells, int max_seq) : module(module), sigmap(module),
			cells(module->selected_cells()), workset(unproven_equiv_cells), satgen(&ez, &sigmap), max_seq(max_seq), success_counter(0)
	{
	}

	void create_timestep(int step)
	{
		vector<int> ez_equal_terms;
		for (auto cell : cells) {
			if (!satgen.importCell(cell, step) && !cell_warn_cache.count(cell)) {
				log_warning("No SAT model available for cell %s (%s).\n", log_id(cell), log_id(cell->type));
				cell_warn_cache.insert(cell);
			}
			if (cell->type == "$equiv") {
				SigBit bit_a = sigmap(cell->getPort("\\A")).to_single_sigbit();
				SigBit bit_b = sigmap(cell->getPort("\\B")).to_single_sigbit();
				if (bit_a != bit_b) {
					int ez_a = satgen.importSigBit(bit_a, step);
					int ez_b = satgen.importSigBit(bit_b, step);
					ez_equal_terms.push_back(ez.IFF(ez_a, ez_b));
				}
			}
		}

		log_assert(!ez_step_is_consistent.count(step));
		ez_step_is_consistent[step] = ez.expression(ez.OpAnd, ez_equal_terms);
	}

	void run()
	{
		log("Found %d unproven $equiv cells in module %s:\n", GetSize(workset), log_id(module));

		create_timestep(1);
		for (int step = 1; step <= max_seq; step++)
		{
			ez.assume(ez_step_is_consistent[step]);

			log("  Proving existence of base case for step %d. (%d clauses over %d variables)\n", step, ez.numCnfClauses(), ez.numCnfVariables());
			if (!ez.solve()) {
				log("  Proof for base case failed. Circuit inherently diverges!\n");
				break;
			}

			create_timestep(step+1);
			int new_step_not_consistent = ez.NOT(ez_step_is_consistent[step+1]);
			ez.bind(new_step_not_consistent);

			log("  Proving induction step %d. (%d clauses over %d variables)\n", step, ez.numCnfClauses(), ez.numCnfVariables());
			if (!ez.solve(new_step_not_consistent)) {
				log("  Proof for induction step holds. Entire workset of %d cells proven!\n", GetSize(workset));
				for (auto cell : workset)
					cell->setPort("\\B", cell->getPort("\\A"));
				success_counter += GetSize(workset);
				return;
			}

			log("  Proof for induction step failed. %s\n", step != max_seq ? "Extending to next time step." : "Trying to prove individual $equiv from workset.");
		}

		for (auto cell : workset)
		{
			SigBit bit_a = sigmap(cell->getPort("\\A")).to_single_sigbit();
			SigBit bit_b = sigmap(cell->getPort("\\B")).to_single_sigbit();

			int ez_a = satgen.importSigBit(bit_a, max_seq+1);
			int ez_b = satgen.importSigBit(bit_b, max_seq+1);

			log("  Trying to prove $equiv for %s:", log_signal(sigmap(cell->getPort("\\Y"))));
			if (!ez.solve(ez.XOR(ez_a, ez_b))) {
				log(" success!\n");
				cell->setPort("\\B", cell->getPort("\\A"));
				success_counter++;
			} else {
				log(" failed.\n");
			}
		}
	}
};

struct EquivInductPass : public Pass {
	EquivInductPass() : Pass("equiv_induct", "proving $equiv cells using temporal induction") { }
	virtual void help()
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    equiv_induct [options] [selection]\n");
		log("\n");
		log("Uses a version of temporal induction to prove $equiv cells.\n");
		log("\n");
		log("Only selected $equiv cells are proven and only selected cells are used to\n");
		log("perform the proof.\n");
		log("\n");
		log("    -seq <N>\n");
		log("        the max. number of time steps to be considered (default = 4)\n");
		log("\n");
		log("This command is very effective in proving complex sequential circuits, when\n");
		log("the internal state of the circuit quickly propagates to $equiv cells.\n");
		log("\n");
		log("However, this command uses a weak definition of 'equivalence': This command\n");
		log("proves that the two circuits will not diverge after they produce equal\n");
		log("outputs (observable points via $equiv) for at least <N> cycles (the <N>\n");
		log("specified via -seq).\n");
		log("\n");
		log("Combined with simulation this is very powerful because simulation can give\n");
		log("you confidence that the circuits start out synced for at least <N> cycles\n");
		log("after reset.\n");
		log("\n");
	}
	virtual void execute(std::vector<std::string> args, Design *design)
	{
		int success_counter = 0;
		int max_seq = 4;

		log_header("Executing EQUIV_INDUCT pass.\n");

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-seq" && argidx+1 < args.size()) {
				max_seq = atoi(args[++argidx].c_str());
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		for (auto module : design->selected_modules())
		{
			pool<Cell*> unproven_equiv_cells;

			for (auto cell : module->selected_cells())
				if (cell->type == "$equiv") {
					if (cell->getPort("\\A") != cell->getPort("\\B"))
						unproven_equiv_cells.insert(cell);
				}

			if (unproven_equiv_cells.empty()) {
				log("No selected unproven $equiv cells found in %s.\n", log_id(module));
				continue;
			}

			EquivInductWorker worker(module, unproven_equiv_cells, max_seq);
			worker.run();
			success_counter += worker.success_counter;
		}

		log("Proved %d previously unproven $equiv cells.\n", success_counter);
	}
} EquivInductPass;

PRIVATE_NAMESPACE_END
