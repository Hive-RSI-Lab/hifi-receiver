* 2N3904 (Fairchild SPICE, as round 2). BF is a parameter for the hFE corner
* (JSCJ/CJ MMBT3904 hFE min 40 at 0.1 mA, 70 at 1 mA).
.model Q3904 NPN(IS=6.734f XTI=3 EG=1.11 VAF=74.03 BF={BFQ} NE=1.259 ISE=6.734f IKF=66.78m XTB=1.5 BR=.7371 NC=2 ISC=0 IKR=0 RC=1 CJC=3.638p MJC=.3085 VJC=.75 FC=.5 CJE=4.493p MJE=.2593 VJE=.75 TR=239.5n TF=301.2p ITF=.4 VTF=4 XTF=2 RB=10)
* JSCJ 2N7002 (C8545, LCSC datasheet): Vth 1.0/1.6/2.5 V at 250 uA; RDS(on) 1.1 typ / 7 ohm max at
* VGS 5 V, 50 mA. Level 1: Rds ~ 1/(KP (Vgs-VTO)), KP chosen per corner.
.subckt N7002 d g s vto=1.6 kp=0.27
M1 d g s s NM L=1u W=1u
D1 s d DB
.model NM NMOS(LEVEL=1 VTO={vto} KP={kp} LAMBDA=0.01 CGSO=20p CGDO=5p)
.model DB D(IS=1e-12 N=1.1 RS=0.5 CJO=20p)
.ends
