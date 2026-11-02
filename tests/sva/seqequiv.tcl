yosys -import

set abc_exec "yosys-abc"
if {[info exists ::env(YOSYS_ABC)] && $::env(YOSYS_ABC) ne ""} {
	set abc_exec $::env(YOSYS_ABC)
}

proc module_list {} {
	set stat [yosys tee -q -s result.json stat -json A:top]
	return [dict keys [dict get $stat modules]]
}

# ABC leaves a history file in its working directory, so we keep it and the AIGs
# out of the source tree
set srcdir [pwd]
set workdir [exec mktemp -d]
cd $workdir

try {
	foreach path [glob -directory $srcdir *.sv] {
		set fn [file tail $path]
		log -header "Testset $fn"
		log -push
		design -reset

		read_slang $path
		check -assert

		foreach m [module_list] {
			log -header "Testcase $m"
			log -push
			test_slangsva
			async2sync
			formalff -clk2ff
			techmap
			aigmap
			opt_clean
			# Strip backslash from the start of $m
			set m [string range $m 1 end]
			write_aiger -zinit "$fn-$m.aig"
			# Insist on a positive verdict so a missing solver can't pass for a proof
			yosys exec -expect-return 0 -expect-stdout "Property proved" -- $abc_exec -c "\"read $fn-$m.aig; pdr -v\""

			log -pop
		}
		log -pop
	}
} finally {
	cd $srcdir
	file delete -force $workdir
}
