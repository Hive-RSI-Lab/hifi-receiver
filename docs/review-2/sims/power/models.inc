* power-review models (review-2, 2026-10-28). Fitted to datasheet figures, not vendor models.
* AO4485 (AOS Rev1.1 Mar 2024): Vth -1.7/-1.9/-2.5 V @250uA, ~20 A @ Vgs -3 V Vds -5 V (fig 1),
* Ciss 2500 pF, Crss 180 pF @ 20 V, Rg 4 ohm. VTO is a parameter for corners.
.param vto_p=1.9
.model AO4485 VDMOS pchan Vto={-vto_p} Kp=33 Rs=2m Rd=6m Rg=4 lambda=0.01
+ Cgdmax=1.5n Cgdmin=0.15n a=0.6 Cgs=2.3n Cjo=0.9n m=0.5 Is=2e-11 N=1.1 Rb=10m ksubthres=0.12 mtriode=1.2
.model N2N7002 VDMOS nchan Vto=1.6 Kp=0.6 Rs=0.5 Rd=0.5 Cgdmax=30p Cgdmin=5p Cgs=25p Cjo=20p Is=1e-12 ksubthres=0.1
.model Q3904 NPN(IS=6.734f BF=200 VAF=74 IKF=66.78m ISE=6.734f NE=1.259 BR=0.7371 RB=10 RC=1 CJE=4.493p CJC=3.638p TF=301.2p TR=239.5n)
.model QS8050 NPN(IS=1e-14 BF=250 VAF=100 IKF=0.5 RB=5 RC=0.3 CJE=20p CJC=10p)
.model DZ15 D(Is=1e-14 N=1 Rs=10 BV=15 IBV=5m Cjo=50p)
.model DZ33 D(Is=1e-14 N=1 Rs=40 BV=33 IBV=5m NBV=8 Cjo=30p)
.model DZ33LO D(Is=1e-14 N=1 Rs=40 BV=31 IBV=5m NBV=8 Cjo=30p)
.model DBODY D(Is=1e-11 N=1)
