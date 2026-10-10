# Tools to run at a machine

These read a running or stopped machine over its monitor, its serial port, or
its debugger. They are for finding out what happened, and none of them is part
of `make check`.

`tests/deploy/` holds the checks and what they need. The difference is who
starts it: a check runs from `make`, and one of these runs when a person has a
question.

| Tool | The question it answers |
|---|---|
| `bugcheck_probe.py` | What did the machine stop on, and what did the payload leave behind |
| `death_report.py` | What faulted first, and what the payload's regions were mapped as |
| `snapshot.py` | Everything worth keeping, out of a stopped machine before it is gone |
| `watch.py` | Is the machine moving, and is it a hang or a stop |
| `hotloop.py` | Which site the machine is spinning on |
| `reg_history.py` | What the exception registers did over a run |
| `handover_probe.py` | What the transfer to the kernel did |
| `bisect_list.py` | Which line of a patch list changes the behaviour |
| `waitlog.py` | Wait for a pattern in a log, and stop when the machine does |
| `enumerate_sites.py` | Write a patch list from an image, offline |
| `identity_probe.sh` | What is still reachable once the kernel is running |
| `read_vbar.sh` | Read VBAR_EL1 from a running guest |
| `spin_at.sh` | How far the kernel gets, by stopping it at an RVA |
| `trap_probe.sh` | Where an undefined instruction is delivered |
| `variant.sh` | Which part of the driver breaks the boot manager |

`logtext.py` and `pool_dump.py` are the readers these share. They live in
`tests/deploy/` with the checks, and these tools put that directory on the
import path.

Run them from the repository root. They use `build/` for their working files.
