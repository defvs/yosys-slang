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
			// The checks of each test and ref, the first one named by the label. A property
			// lowered to several checks has the others named after it, as `<label>$...`.
			dict<std::string, std::vector<RTLIL::Cell *>> refs;
			dict<std::string, std::vector<RTLIL::Cell *>> tests;

			for (auto cell : m->cells()) {
				if (cell->type == ID($check)) {
					std::string name = cell->name.str();
					std::string label = name.substr(0, name.find('$', 1));
					if (!label.ends_with("_test") && !label.ends_with("_ref"))
						log_error("Assertion '%s' is neither test nor ref\n", log_id(cell));
					auto &checks = label.ends_with("_test") ? tests[label] : refs[label];
					if (name == label)
						checks.insert(checks.begin(), cell);
					else
						checks.push_back(cell);
				} else if (cell->type == ID($assert)) {
					log_error("Assertion '%s' is of unsupported '%s' type\n", log_id(cell),
							log_id(cell->type));
				}
			}

			for (auto &[ref_label, ref_checks] : refs) {
				// Find the test counterpart
				std::string prefix = ref_label.substr(0, ref_label.size() - 4);
				auto test_it = tests.find(prefix + "_test");
				if (test_it == tests.end()) {
					log_error("No test assertion '%s_test' found to pair with reference '%s'",
							prefix.c_str(), ref_label.c_str());
				}
				std::vector<RTLIL::Cell *> test_checks = test_it->second;
				tests.erase(test_it);

				RTLIL::Cell *ref = ref_checks.front();
				RTLIL::Cell *test = test_checks.front();
				if (ref->name.str() != ref_label || test->name.str() != prefix + "_test") {
					log_error("No assertion named '%s' or '%s_test'\n", ref_label.c_str(),
							prefix.c_str());
				}

				for (auto cell : ref_checks)
					cell->parameters.erase(ID::PRIORITY);
				for (auto cell : test_checks)
					cell->parameters.erase(ID::PRIORITY);

				for (auto checks : {&ref_checks, &test_checks}) {
					for (auto cell : *checks) {
						if (cell->parameters != ref->parameters) {
							log_error("Assertions '%s' and '%s' differ in parameters\n",
									log_id(cell), log_id(ref));
						}
						if (cell->getPort(ID::TRG) != ref->getPort(ID::TRG)) {
							log_error("Assertions '%s' and '%s' differ in TRG port\n", log_id(cell),
									log_id(ref));
						}
					}
				}

				// A cover acts on `A` being high, asserts and assumes on it being low
				auto flavor = ref->getParam(ID::FLAVOR).decode_string();
				bool is_cover = flavor == "cover";
				auto acts = [&](RTLIL::Cell *cell) {
					RTLIL::SigSpec a = cell->getPort(ID::A);
					return m->And(NEW_ID, cell->getPort(ID::EN), is_cover ? a : m->Not(NEW_ID, a));
				};
				// The checks of an assertion or an assumption act together, it fails
				// where one of them does
				auto acts_any = [&](const std::vector<RTLIL::Cell *> &checks) {
					RTLIL::SigSpec any;
					for (auto cell : checks)
						any.append(acts(cell));
					return checks.size() == 1 ? any : m->ReduceOr(NEW_ID, any);
				};

				RTLIL::Cell *compare = m->addCell(prefix + "_compare", ID($check));

				// What we prove is the match, whichever flavor the pair is of
				compare->parameters = ref->parameters;
				compare->setParam(ID::FLAVOR, std::string("assert"));
				compare->setParam(ID::PRIORITY, 0);
				compare->setPort(ID::TRG, ref->getPort(ID::TRG));
				compare->setPort(ID::EN, RTLIL::S1);
				compare->setPort(ID::ARGS, {});
				if ((flavor != "assert" && flavor != "assume") &&
						(ref_checks.size() != 1 || test_checks.size() != 1)) {
					log_error("Assertions '%s' and '%s' have several checks of flavor %s\n",
							log_id(test), log_id(ref), flavor.c_str());
				} else if (flavor == "live" || flavor == "fair") {
					// Justice goes by the goal and the enable over the whole trace, rather
					// than by the ticks the check acts on, so both have to match
					compare->setPort(ID::A,
							m->And(NEW_ID,
									m->Eqx(NEW_ID, test->getPort(ID::A), ref->getPort(ID::A)),
									m->Eqx(NEW_ID, test->getPort(ID::EN), ref->getPort(ID::EN))));
				} else {
					compare->setPort(
							ID::A, m->Eqx(NEW_ID, acts_any(test_checks), acts_any(ref_checks)));
				}

				for (auto cell : ref_checks)
					m->remove(cell);
				for (auto cell : test_checks)
					m->remove(cell);
			}

			// Any surviving test assertions are unmatched extras
			for (auto &it : tests) {
				log_error("No ref assertion found to pair with test '%s'", it.first.c_str());
			}
		}
	}
} TestSlangsvaPass;

}; // namespace
