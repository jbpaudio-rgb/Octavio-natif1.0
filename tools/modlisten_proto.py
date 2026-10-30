"""the prototype's Vactrol parameters from native PV values"""
def vac_proto(pv):
    """native Vactrol PV dict (thousandths) -> the prototype's parameters"""
    d = dict(fold=.25, sym=.1, decay=.002 * 1000 ** .75, mode=1, wave=0, mwave=0, ratio=2, fm=0, freq=20 * 200 ** .485, ead=1,
             atk=.001 * 2000 ** .21, edec=.005 * 600 ** .64, open=0, fenv=0, lvl=1)
    f = lambda v: v / 1000.0
    for k, v in pv.items():
        if k == 'FOLD': d['fold'] = f(v)
        elif k == 'SYM': d['sym'] = f(v)
        elif k == 'DECAY': d['decay'] = .002 * 1000 ** f(v)
        elif k == 'MODE': d['mode'] = v
        elif k == 'WAVE': d['wave'] = v
        elif k == 'MWAVE': d['mwave'] = v
        elif k == 'RATIO': d['ratio'] = f(v)
        elif k == 'FM': d['fm'] = f(v)
        elif k == 'FENV': d['fenv'] = f(v)
        elif k == 'EAD': d['ead'] = v
        elif k == 'ATK': d['atk'] = .001 * 2000 ** f(v)
        elif k == 'EDEC': d['edec'] = .005 * 600 ** f(v)
        elif k == 'OPEN': d['open'] = f(v)
        elif k == 'FREQ': d['freq'] = 20 * 200 ** f(v)
    return d

