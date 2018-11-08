set testdir [file dirname $argv0]
source $testdir/malloc_common.tcl
source $testdir/lock_common.tcl

proc rowlock_init_test {} {
  sqlite3_enable_shared_cache 1

  faultsim_delete_and_reopen

  if { ![info exists ::G(switchmode)] } {
    set ::G(switchmode) "nm"
  }
  if { $::G(switchmode) == "nm" || $::G(switchmode) == "mp" } {

    if { $::G(switchmode) == "nm" } {
      puts "NORMAL MODE"
      proc code1 {tcl} { uplevel #0 $tcl }
      proc code2 {tcl} { uplevel #0 $tcl }
      proc code3 {tcl} { uplevel #0 $tcl }
      } elseif { $::G(switchmode) == "mp" } {
        puts "MULTIPLE PROCESS MODE"
        proc code1 {tcl} { uplevel #0 $tcl }
        set ::code2_chan [launch_testfixture]
        set ::code3_chan [launch_testfixture]
        proc code2 {tcl} { testfixture $::code2_chan $tcl }
        proc code3 {tcl} { testfixture $::code3_chan $tcl }
        code2 { sqlite3_enable_shared_cache 1 }
        code3 { sqlite3_enable_shared_cache 1 }
      }

      code2 { sqlite3 db2 test.db }
      code3 { sqlite3 db3 test.db }

      proc sql1 {sql} { db eval $sql }
      proc sql2 {sql} { code2 [list db2 eval $sql] }
      proc sql3 {sql} { code3 [list db3 eval $sql] }

      } elseif { $::G(switchmode) == "mt" } {
        puts "MULTIPLE THREAD MODE"
        thread_create A test.db
        thread_create B test.db
        thread_create C test.db
        
        proc catch_return {id sql} {
          set tmpresult [ thread_exec $id $sql ]
          set tmpresult [ split $tmpresult "_" ]
          if { [ lindex $tmpresult 0 ] == "0" } {
            lindex $tmpresult 1
            } else {
              error [ lindex $tmpresult 1 ]
            }
          } 

          proc sql1 {sql} { catch_return A $sql }
          proc sql2 {sql} { catch_return B $sql }
          proc sql3 {sql} { catch_return C $sql }
        }

        proc csql1 {sql} { list [catch { sql1 $sql } msg] $msg }
        proc csql2 {sql} { list [catch { sql2 $sql } msg] $msg }
        proc csql3 {sql} { list [catch { sql3 $sql } msg] $msg }
      }



      proc rowlock_do_sub_test {cmd expected} {

        if {[catch {uplevel #0 "$cmd;\n"} result]} {
          return "\n! Error command: $cmd\nError: $result"
          } else {
            if {[regexp {^[~#]?/.*/$} $expected]} {
      # "expected" is of the form "/PATTERN/" then the result if correct if
      # regular expression PATTERN matches the result.  "~/PATTERN/" means
      # the regular expression must not match.
      if {[string index $expected 0]=="~"} {
        set re [string range $expected 2 end-1]
        if {[string index $re 0]=="*"} {
          # If the regular expression begins with * then treat it as a glob instead
          set ok [string match $re $result]
          } else {
            set re [string map {# {[-0-9.]+}} $re]
            set ok [regexp $re $result]
          }
          set ok [expr {!$ok}]
          } elseif {[string index $expected 0]=="#"} {
        # Numeric range value comparison.  Each term of the $result is matched
        # against one term of $expect.  Both $result and $expected terms must be
        # numeric.  The values must match within 10%.  Or if $expected is of the
        # form A..B then the $result term must be between A and B.
        set e2 [string range $expected 2 end-1]
        foreach i $result j $e2 {
          if {[regexp {^(-?\d+)\.\.(-?\d)$} $j all A B]} {
            set ok [expr {$i+0>=$A && $i+0<=$B}]
            } else {
              set ok [expr {$i+0>=0.9*$j && $i+0<=1.1*$j}]
            }
            if {!$ok} break
          }
          if {$ok && [llength $result]!=[llength $e2]} {set ok 0}
          } else {
            set re [string range $expected 1 end-1]
            if {[string index $re 0]=="*"} {
          # If the regular expression begins with * then treat it as a glob instead
          set ok [string match $re $result]
          } else {
            set re [string map {# {[-0-9.]+}} $re]
            set ok [regexp $re $result]
          }
        }
        } elseif {[regexp {^~?\*.*\*$} $expected]} {
      # "expected" is of the form "*GLOB*" then the result if correct if
      # glob pattern GLOB matches the result.  "~/GLOB/" means
      # the glob must not match.
      if {[string index $expected 0]=="~"} {
        set e [string range $expected 1 end]
        set ok [expr {![string match $e $result]}]
        } else {
          set ok [string match $expected $result]
        }
        } else {
          set ok [expr {[string compare $result $expected]==0}]
        }
        if {!$ok} {
          return "! Error command: $cmd\n! expected: \[$expected\]\n! got:      \[$result\]"
          } else {
            return "Ok"
          }
        }
      }


      proc rowlock_do_test {name cmd} {

  # global argv cmdlinearg

  fix_testname name

  sqlite3_memdebug_settitle $name

  if {[info exists ::G(perm:prefix)]} {
    set name "$::G(perm:prefix)$name"
  }

  incr_ntest
  output1 -nonewline $name...
  # output1 $name...
  flush stdout

  if {![info exists ::G(match)] || [string match $::G(match) $name]} {
    catch {uplevel #0 "$cmd;\n"} ret
    set fail_num 0
    set error_str ""
    foreach item $ret {
      if { $item != "Ok" } {
        # puts $item
        set item [ string trim $item ]
        incr fail_num
        set error_str "$error_str\n$item"
      }
    }
    if { $fail_num > 0 } {
      fail_test $name
      puts "$error_str"
      } else {
        puts "Ok"
      }	
      } else {
        output1 " Omitted"
        omit_test $name "pattern mismatch" 0
      }

      flush stdout
    }


    proc rowlock_finish_test {} {
      sqlite3_enable_shared_cache 0
      catch { thread_halt * }
      catch { code2 { db2 close } }
      catch { code3 { db3 close } }
      catch { close $::code2_chan }
      catch { close $::code3_chan }
      catch { db close }

    }