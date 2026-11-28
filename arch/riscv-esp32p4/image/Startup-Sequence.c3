Echo "AROS esp32p4-riscv, read-only bring-up"
Echo "SYS: is mounted read-only on purpose; transient state belongs in RAM:"

Assign "T:" "RAM:"
Assign "ENVARC:" "SYS:Prefs/Env-Archive"
; ENV: is the working copy in RAM:, filled from ENVARC: as the standard
; Startup-Sequence does. A multi-directory ENV: (RAM: plus ENVARC:) reads,
; but notification follows only its first directory, so IPrefs would miss
; a saved file.
Assign "ENV:" "RAM:"
C:Copy >NIL: "ENVARC:" "ENV:" ALL NOPRO NOREQ QUIET
Assign "LIBS:" "SYS:Classes" ADD
Assign "THEMES:" "SYS:Prefs/Presets/Themes"
Assign "THEME:" "THEMES:@C3_THEME@"
Assign "WANDERER:" "SYS:System/Wanderer" DEFER

Echo "Applying preferences"
C:IPrefs

Echo "Registering the PNG datatype used by theme images"
C:AddDataTypes DEVS:DataTypes/PNG QUIET

Echo "Launching Wanderer from read-only SYS:"
WANDERER:Wanderer

If NOT ERROR
    EndCLI
EndIf
