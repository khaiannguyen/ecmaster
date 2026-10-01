# EtherCAT Master — Jetson Orin Nano + Intel i226


## Input freshness (`ecm_run --fresh`, GD9.8)

A correct working counter only says that every slave *processed* the frame.
It does not say that the inputs are new: a slave whose application stalls
keeps answering with the same data. `ecm_run` can catch that when the slave's
PDO contract has an application counter in its inputs, which the slave
increments every cycle:

    --fresh all=0              16-bit counter at input byte 0 of every slave
    --fresh 1=0,2=off,5=4:8    slave 1: byte 0; slave 2: no counter; slave 5: 8-bit at byte 4
    --fresh-offset 0           same as --fresh all=0 (GD7.4)

Entries apply left to right; a later one overrides. A counter outside a
slave's inputs is refused at startup.

**A slave with `off` (or not listed) is not checked: for that slave the master
cannot detect "WKC correct but data old".** `ecm_run` prints this for every
such slave at startup. Commercial drives usually have no such counter, so
they are `off`; use the drive's own status (statusword toggle bits, EMCY)
in the application layer instead.
