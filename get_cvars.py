import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def read(name):
    return (ROOT / name).read_text(encoding='utf-8', errors='replace')


src = read('source/src/insbots.cpp')
m = re.search(r'#define\s+INSBOTS_VERSION\s+"([^"]+)"', src)
if not m:
    raise SystemExit('INSBOTS_VERSION not found')
version = m.group(1)

cvars = {}  # name -> (default, help)


def select_default_platform_branches(content):
    """Select the non-Linux half of simple platform CVar declarations.

    cvars.txt is the portable/default administrator reference. Parsing both
    halves of an #ifdef _LINUX declaration creates a false duplicate, while
    the Linux-only values are documented in linux/offset-audit/OFFSETS.md.
    Repeat the substitution because a source file may contain several blocks.
    """
    simple_linux_ifelse = re.compile(
        r'^#ifdef\s+_LINUX\s*\r?\n'
        r'((?:(?!^#(?:if|else|endif)\b)[\s\S])*?)'
        r'^#else\s*\r?\n'
        r'((?:(?!^#(?:if|else|endif)\b)[\s\S])*?)'
        r'^#endif\s*$',
        re.M)
    while True:
        content, count = simple_linux_ifelse.subn(lambda match: match.group(2), content)
        if count == 0:
            return content

# 1) Direct CfgVar declarations from every source/src/*.cpp file.
for cpp in sorted((ROOT / 'source' / 'src').glob('*.cpp')):
    content = cpp.read_text(encoding='utf-8', errors='replace')
    content = select_default_platform_branches(content)
    for cm in re.finditer(
        r'CfgVar\s+[A-Za-z0-9_]+\(\s*"([A-Za-z0-9_]+)",\s*"([^"]*)",\s*"([^"]*)"\s*\);',
        content):
        name, default, help_ = cm.group(1), cm.group(2), cm.group(3)
        if name in cvars:
            raise SystemExit('duplicate cvar: ' + name)
        cvars[name] = (default, help_)

bot_src = read('source/src/bot.cpp')


def block(pattern):
    start = re.search(pattern, bot_src, re.M)
    if not start:
        raise SystemExit('pattern not found: ' + pattern)
    end = bot_src.find('};', start.end())
    if end < 0:
        raise SystemExit('unterminated block: ' + pattern)
    return bot_src[start.end():end]


# 2) Dynamic reload_safe_pct_<weapon> entries plus the default.
weapon_block = block(r'static WeaponProfile\s+s_wp\s*\[\]\s*=\s*\{')
weapons = re.findall(r'\{\s*"([A-Za-z0-9_]+)",', weapon_block)
for w in weapons:
    name = 'reload_safe_pct_' + w
    if name in cvars:
        raise SystemExit('duplicate dynamic cvar: ' + name)
    cvars[name] = ('0.60', 'Safe reload threshold (pct ammo remaining) for weapon')
if 'reload_safe_pct_default' in cvars:
    raise SystemExit('duplicate dynamic cvar: reload_safe_pct_default')
cvars['reload_safe_pct_default'] = ('0.60', 'Default safe reload threshold')

# 3) Dynamic combat-profile settings from NewCombatCfg.
combat_block = block(r'static CombatProfile\s+s_combat\[WC_COUNT\]\s*=\s*\{')
profiles = re.findall(r'\{\s*"([A-Za-z0-9_]+)",', combat_block)

defaults_block = block(
    r'static const char \*\s*s_combatDefaults\[WC_COUNT\]\[7\]\s*=\s*\{')
rows = []
for row in re.finditer(r'\{([^{}]*)\}', defaults_block):
    vals = re.findall(r'"([^"]*)"', row.group(1))
    if len(vals) != 7:
        raise SystemExit('combat default row does not have 7 values')
    rows.append(vals)
if len(rows) != len(profiles):
    raise SystemExit('combat default row count mismatch')

modes_block = block(
    r'static const char \*\s*s_combatRecoilModeDefaults\[WC_COUNT\]\s*=\s*\{')
modes = re.findall(r'"([^"]*)"', modes_block)
percents_block = block(
    r'static const char \*\s*s_combatRecoilPercentDefaults\[WC_COUNT\]\s*=\s*\{')
percents = re.findall(r'"([^"]*)"', percents_block)
if len(modes) != len(profiles) or len(percents) != len(profiles):
    raise SystemExit('combat recoil default count mismatch')

settings = [
    ('detect_range', 0, 'maximum enemy detection range; -1 inherits vis_range'),
    ('engage_range', 1, 'maximum range at which this weapon class may fire; -1 inherits vis_range'),
    ('stop_dist', 2, 'distance at which this weapon class stops advancing; -1 inherits stop_dist'),
    ('reaction', 3, 'reaction delay before firing; -1 inherits reaction'),
    ('aim_error', 4, 'aim error in degrees; -1 inherits aim_error'),
    ('aim_speed', 5, 'maximum aim turn speed in degrees/sec; -1 inherits aim_speed'),
    ('reserve_mags', 6, 'number of reserve magazines this weapon class may reload per life'),
]
for i, prof in enumerate(profiles):
    for suffix, idx, help_ in settings:
        name = prof + '_' + suffix
        if name in cvars:
            raise SystemExit('duplicate dynamic cvar: ' + name)
        cvars[name] = (rows[i][idx], help_)
    for suffix, default, help_ in [
        ('recoil_control', modes[i], 'recoil mode: inherit, realistic, perfect, aimerror, or percent'),
        ('recoil_percent', percents[i], 'direct recoil compensation percent (0-100); -1 follows recoil_control'),
    ]:
        name = prof + '_' + suffix
        if name in cvars:
            raise SystemExit('duplicate dynamic cvar: ' + name)
        cvars[name] = (default, help_)

out = []
out.append('================================================================================')
out.append('  INSBOTS v' + version + ' - CVAR LIST')
out.append('================================================================================')
out.append('You can execute these commands in your server console or config file.')
out.append('Plugin cvars use:   insbot set <name> <value>')
out.append('================================================================================')
out.append('')
out.append('// Generated by get_cvars.py from source CfgVar declarations.')
out.append('')
for name in sorted(cvars):
    default, help_ = cvars[name]
    out.append('insbot set ' + name.ljust(25) + ' "' + default + '" // ' + help_)
out.append('')
(ROOT / 'cvars.txt').write_bytes(('\r\n'.join(out)).encode('utf-8'))
print('Wrote ' + str(len(cvars)) + ' cvars to cvars.txt for v' + version)
