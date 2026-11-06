//
// sv-elab
//
// Copyright Martin Povišer <povik@cutebit.org>
// Distributed under the terms of the ISC license, see LICENSE
//
#include "kernel/rtlil.h"

namespace {

USING_YOSYS_NAMESPACE

struct TestSlangsvaPass : Pass
{
	TestSlangsvaPass() : Pass("test_slangsva", "helper pass used by sv-elab SVA support testing") {}

	void help() override
	{
		log("Perform manipulations in support of sv-elab SVA support testing.\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *d) override
	{
		log_header(d, "Executing TEST_SLANGSVA pass.\n");

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			break;
		}
		extra_args(args, argidx, d);

		for (auto m : d->modules()) {
			pool<RTLIL::Cell *> refs;
			pool<RTLIL::Cell *> tests;

			for (auto cell : m->cells()) {
				if (cell->type == ID($check)) {
					if (cell->name.ends_with("_test")) {
						tests.insert(cell);
					} else if (cell->name.ends_with("_ref")) {
						refs.insert(cell);
					} else {
						log_error("Assertion '%s' is neither test nor ref\n", log_id(cell));
					}
				} else if (cell->type == ID($assert)) {
					log_error("Assertion '%s' is of unsupported '%s' type\n", log_id(cell),
							log_id(cell->type));
				}
			}

			for (auto ref : refs) {
				// Find the test counterpart
				auto test_name = ref->name.substr(0, ref->name.size() - 4) + "_test";

				auto test = m->cell(test_name);
				if (!test || !tests.count(test)) {
					log_error("No test assertion '%s' found to pair with reference '%s'",
							test_name.c_str(), log_id(ref));
				}
				tests.erase(test);

				ref->parameters.erase(ID::PRIORITY);
				test->parameters.erase(ID::PRIORITY);

				if (ref->parameters != test->parameters) {
					log_error("Assertions '%s' and '%s' differ in parameters\n", log_id(test),
							log_id(ref));
				}

				if (ref->getPort(ID::TRG) != test->getPort(ID::TRG)) {
					log_error("Assertions '%s' and '%s' differ in TRG port\n", log_id(test),
							log_id(ref));
				}

				// A cover acts on `A` being high, asserts and assumes on it being low
				bool is_cover = ref->getParam(ID::FLAVOR).decode_string() == "cover";
				auto acts = [&](RTLIL::Cell *cell) {
					RTLIL::SigSpec a = cell->getPort(ID::A);
					return m->And(NEW_ID, cell->getPort(ID::EN), is_cover ? a : m->Not(NEW_ID, a));
				};

				RTLIL::Cell *compare = m->addCell(
						ref->name.substr(0, ref->name.size() - 4) + "_compare", ID($check));

				// What we prove is the match, whichever flavor the pair is of
				compare->parameters = ref->parameters;
				compare->setParam(ID::FLAVOR, std::string("assert"));
				compare->setParam(ID::PRIORITY, 0);
				compare->setPort(ID::TRG, ref->getPort(ID::TRG));
				compare->setPort(ID::EN, RTLIL::S1);
				compare->setPort(ID::ARGS, {});
				compare->setPort(ID::A, m->Eqx(NEW_ID, acts(test), acts(ref)));

				m->remove(ref);
				m->remove(test);
			}

			// Any surviving test assertions are unmatched extras
			for (auto test : tests) {
				log_error("No ref assertion found to pair with test '%s'", log_id(test));
			}
		}
	}
} TestSlangsvaPass;

}; // namespace
