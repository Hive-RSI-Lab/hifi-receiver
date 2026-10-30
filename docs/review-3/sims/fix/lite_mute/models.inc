* generic models: 2N3904 (Fairchild SPICE), 2N7002 (level 1 fit to onsemi 2N7002: Vth 2.1 V typ, Rds(on) ~2.5 ohm at 4.5 V, body diode)
.model Q3904 NPN(IS=6.734f XTI=3 EG=1.11 VAF=74.03 BF=416.4 NE=1.259 ISE=6.734f IKF=66.78m XTB=1.5 BR=.7371 NC=2 ISC=0 IKR=0 RC=1 CJC=3.638p MJC=.3085 VJC=.75 FC=.5 CJE=4.493p MJE=.2593 VJE=.75 TR=239.5n TF=301.2p ITF=.4 VTF=4 XTF=2 RB=10)
.subckt N7002 d g s vto=2.1
M1 d g s s NM L=1u W=1u
D1 s d DB
.model NM NMOS(LEVEL=1 VTO={vto} KP=0.25 LAMBDA=0.01 CGSO=20p CGDO=5p)
.model DB D(IS=1e-12 N=1.1 RS=0.5 CJO=20p)
.ends
