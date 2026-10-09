start_server {tags {"ycsb" "external:skip"}} {
    set spec [file normalize [file join [lindex [r config get dir] 1] ycsb.spec]]

    test {YCSB is registered without loadmodule} {
        assert_equal "ycsb.run" [lindex [lindex [r command info ycsb.run] 0] 0]
    }

    test {YCSB rejects wrong arity and missing files} {
        assert_error "*wrong number of arguments*" {r ycsb.run}
        assert_error "*wrong number of arguments*" {r ycsb.run missing extra}
        assert_error "*File not open*" {r ycsb.run ${spec}.missing}
        assert_equal PONG [r ping]
    }

    test {YCSB runs a small workload twice} {
        set fd [open $spec w]
        puts $fd "\n# Small workload\nrecordcount=20\noperationcount=20\nrequestdistribution=uniform\nreadproportion=0.5\nupdateproportion=0.5\nscanproportion=0"
        close $fd
        r flushdb
        assert_equal OK [r ycsb.run $spec]
        assert_equal 20 [r dbsize]
        assert_equal OK [r ycsb.run $spec]
        assert_equal 20 [r dbsize]
        assert_equal PONG [r ping]
    }

    test {YCSB rejects unsupported scans before loading records} {
        set fd [open $spec w]
        puts $fd "recordcount=20\noperationcount=20\nscanproportion=1"
        close $fd
        r flushdb
        assert_error "*YCSB scans are not supported*" {r ycsb.run $spec}
        assert_equal 0 [r dbsize]
    }

    test {YCSB returns errors for invalid properties} {
        foreach records {invalid -1 0 1} {
            set fd [open $spec w]
            puts $fd "recordcount=$records\noperationcount=20"
            close $fd
            assert_equal 1 [catch {r ycsb.run $spec}]
            assert_equal PONG [r ping]
        }
    }

    test {YCSB boot mode runs without listeners or persistence and exits} {
        set fd [open $spec w]
        puts $fd "recordcount=20\noperationcount=20\nrequestdistribution=uniform"
        close $fd
        set dir [file dirname $spec]
        set dump [file join $dir ycsb-boot.rdb]
        set fd [open $dump w]
        puts -nonewline $fd "Not an RDB file"
        close $fd
        set output [exec src/redis-server --ycsb-run $spec --port 0 \
            --dir $dir --dbfilename ycsb-boot.rdb --appendonly yes \
            --save "1 1" --daemonize yes]
        assert_match "*YUKON_YCSB_WORKLOAD_OPS=20*" $output
        set fd [open $dump r]
        assert_equal "Not an RDB file" [read $fd]
        close $fd
        assert_equal 0 [file exists [file join $dir appendonlydir]]
        file delete $dump
    }

    test {YCSB boot mode reports failures with a nonzero exit status} {
        assert_equal 1 [catch {exec src/redis-server --ycsb-run ${spec}.missing --port 0} output]
        assert_match "*YCSB command failed*File not open*" $output
        assert_equal 1 [catch {exec src/redis-server --ycsb-run} output]
        assert_match "*requires one workload path*" $output
        assert_equal 1 [catch {exec src/redis-server --ycsb-run $spec --ycsb-run $spec} output]
        assert_match "*cannot repeat*" $output
        assert_equal 1 [catch {exec src/redis-server --ycsb-run $spec --replicaof 127.0.0.1 6379} output]
        assert_match "*requires a standalone primary*" $output
        assert_equal 1 [catch {exec src/redis-server --ycsb-run $spec --cluster-enabled yes --cluster-bus-port-protected-mode no} output]
        assert_match "*requires a standalone primary*" $output
    }

    file delete $spec
}
