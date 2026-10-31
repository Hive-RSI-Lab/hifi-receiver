"""Output Zobel sweep for core's TPA3220 filter (issue #137, docs/boards/core-zobel.md).

One BTL leg: 10 uH (30 mOhm DCR) + 1 uF to GND, the speaker as RL/2 per leg.
For each Zobel (Rz + Cz to GND) prints the peak and the 20 kHz response, open
and at 4/6/8 ohm, and Rz's dissipation at a full-swing (10.6 Vrms/leg) 20 kHz sine.
Needs ngspice on PATH (. ~/modules/bin/eda-env). Writes d.cir/out.txt in the cwd.
"""
import subprocess, re, math, sys, itertools
# One leg (BTL is symmetric; differential load RL -> RL/2 per leg to virtual ground)
def run(rz, cz, rl):
    load = f"RL out 0 {rl/2}" if rl else "RL out 0 1e9"
    zob = f"Rz out z {rz}\nCz z 0 {cz}" if rz else ""
    deck = f"""* leg
V1 in 0 AC 1
L1 in n1 10u
Rdcr n1 out 0.03
C1 out 0 1u
{zob}
{load}
.ac dec 400 1k 500k
.control
run
wrdata out.txt vdb(out)
quit
.endc
.end
"""
    open("d.cir","w").write(deck)
    subprocess.run(["ngspice","-b","d.cir"],capture_output=True)
    pts=[tuple(map(float,l.split()[:2])) for l in open("out.txt")]
    pk=max(pts,key=lambda p:p[1]); a20=min(pts,key=lambda p:abs(p[0]-20e3))[1]
    return pk, a20
for rz,cz in [(0,0),(4.7,1e-6),(3.3,1e-6),(4.7,470e-9),(6.8,470e-9),(3.3,470e-9),(4.7,330e-9),(6.8,220e-9),(10,220e-9)]:
    row=[]
    for rl in [None,4,6,8]:
        pk,a=run(rz,cz,rl); row.append(f"{'open' if not rl else rl}:pk{pk[1]:+.1f}@{pk[0]/1e3:.0f}k a20{a:+.2f}")
    # Zobel power at 20 kHz, leg 10.6 Vrms (clip at 30V PVDD), and at 2 kHz worst music-ish
    p20 = (10.6**2*rz/(rz**2+(1/(2*math.pi*20e3*cz))**2)) if rz else 0
    print(f"Rz={rz} Cz={cz*1e9:.0f}n P20k_full={p20:.2f}W | "+" | ".join(row))
