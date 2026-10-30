* Lite PVDD switch (netlist boards/lite-main/kicad/lite-main.net): Q1 AO4485, R5 100k, R15 1k + C2 100n
* (gate-drain), R6 47k, Q4 MMBT3904 (E = PD_PG), R7 10k, R8 100k, R4 10k, PVDD bulk C58 470u polymer + C59 220u.
* Source: charger + 1 m cable (R 0.1 ohm, L 0.6 uH).
Rcab vsrc vc 0.1
Lcab vc VBUS 0.6u
* VBUS caps C1 1u, C3 10u/50V X5R (~8u at 20 V), C4 4.7u/50V (~3.8u), C5 100n; TPS54302 load ~1.7 W -> 240 ohm
C1 VBUS 0 1u
C3 VBUS 0 8u
C4 VBUS 0 3.8u
C5 VBUS 0 100n
Rbuck VBUS 0 240
* also VBUS sense divider R13/R14 110k
Rvs VBUS 0 110k
M1 PVDDm PSW_G VBUS AO4485
Vfetm PVDD PVDDm 0
R5 VBUS PSW_G {r5}
R15 PSW_G PSW_MILLER 1k
C2 PSW_MILLER PVDD 100n
R6 PSW_G PSW_PULL 220k
Cgsx VBUS PSW_G {cgs}
Q4 PSW_PULL PSW_B PD_PG Q3904
R7 PVDD_EN PSW_B 10k
R8 PVDD_EN 0 100k
R4 PD_PG V3V3 10k
* CH224A PG open drain: closed (low = good) when vpg > 1.5
Spg PD_PGs 0 vpg 0 SWPG
Vpgm PD_PG PD_PGs 0
.model SWPG SW(Ron=30 Roff=1e9 Vt=1.5 Vh=0.1)
* PVDD: C58 470u ESR 16m, C59 220u ESR 0.15, ceramics 3x1u+2x100n+2x1n (~2.6u at 20 V), R16 22k, R17+R18 110k
C58 PVDD p58 470u
R58 p58 0 16m
C59 PVDD p59 220u
R59 p59 0 0.15
Ccer PVDD 0 2.6u
R16 PVDD 0 22k
Rps PVDD 0 110k
