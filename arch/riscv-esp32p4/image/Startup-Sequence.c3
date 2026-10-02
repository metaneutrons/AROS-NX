Echo "AROS esp32p4-riscv, read-only bring-up"
Echo "SYS: is mounted read-only on purpose; transient state belongs in RAM:"

Assign "T:" "RAM:"
Assign "ENV:" "RAM:"
Assign "ENVARC:" "SYS:Prefs/Env-Archive"
Assign "ENV:" "ENVARC:" ADD
Assign "LIBS:" "SYS:Classes" ADD
Assign "THEMES:" "SYS:Prefs/Presets/Themes"
Assign "THEME:" "THEMES:@C3_THEME@"
Assign "WANDERER:" "SYS:System/Wanderer" DEFER

Echo "Registering the PNG datatype used by theme images"
C:AddDataTypes DEVS:DataTypes/PNG QUIET

Echo "Launching Wanderer from read-only SYS:"
WANDERER:Wanderer

If NOT ERROR
    EndCLI
EndIf
