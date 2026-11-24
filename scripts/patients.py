"""patients.py - the virtual-patient catalogue (Pulse states made by tools/pulse_states).

Ids and descriptions must match the catalogue in tools/pulse_states/pulse_states.cpp. The dashboard shows `label`;
`hint` tells the bench operator what to expect and what a clinician would try. Room-air SpO2 and R/C come from
tools/pulse_states (results/patients/states_build.txt).
"""

PATIENTS = [
    # id,              label,                hint
    ("healthy_male",   "Healthy male",       "180 cm. Reference patient: PCV 20/5 gives ~570 mL (7.7 mL/kg)."),
    ("healthy_female", "Healthy female",     "163 cm, ideal weight 55 kg: the same PCV 20/5 is NOT protective."),
    ("obese_male",     "Obese male",         "98 kg but same height as healthy_male: ideal weight decides Vt."),
    ("ards_moderate",  "ARDS, moderate",     "Stiff lungs and shunt: low SpO2 on room air. Try FiO2 and PEEP."),
    ("ards_severe",    "ARDS, severe",       "Very stiff lungs, severe hypoxemia. FiO2, PEEP, protective Vt."),
    ("copd",           "COPD",               "High resistance, gas trapping: watch auto-PEEP; lower RR, longer E."),
    ("pneumonia",      "Pneumonia (right)",  "One lung affected: shunt, lower SpO2. Try FiO2."),
    ("asthma_attack",  "Asthma attack",      "Very high resistance: low Vt in PCV, auto-PEEP at high RR."),
]
IDS = [p[0] for p in PATIENTS]
LABEL = {p[0]: p[1] for p in PATIENTS}
HINT = {p[0]: p[2] for p in PATIENTS}
