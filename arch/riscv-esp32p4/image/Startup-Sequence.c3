Echo "AROS esp32p4-riscv, read-only bring-up"
Echo "SYS: is mounted read-only on purpose; transient state belongs in RAM:"

Assign "T:" "RAM:"
Assign "ENV:" "RAM:"
Assign "ENVARC:" "SYS:Prefs/Env-Archive"
Assign "ENV:" "ENVARC:" ADD
Assign "LIBS:" "SYS:Classes" ADD
Assign "WANDERER:" "SYS:System/Wanderer" DEFER

Echo "Launching Wanderer from read-only SYS:"
WANDERER:Wanderer
Echo "Wanderer exited; initial CLI remains available"
