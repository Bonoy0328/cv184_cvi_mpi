#!/usr/bin/env python3
"""Generate an external, replaceable name-input dictionary from ibus-table data.
The derived TSV retains LGPL-2.1-or-later; see assets/ime/COPYRIGHT and licenses/.
No dictionary code is linked into the application.
"""
from pathlib import Path
import bz2,re,collections
root=Path(__file__).resolve().parents[1]
source=root/'assets/ime/pinyin_table.txt.bz2'
words=collections.defaultdict(dict)
priority='王李张刘陈杨黄赵吴周徐孙马朱胡郭何林高罗郑梁谢宋唐许韩冯邓曹彭曾肖田董袁潘于蒋蔡余杜叶程苏魏吕丁任沈姚卢姜崔钟谭陆汪范金石廖贾夏韦付方白邹孟熊秦邱江尹薛阎段雷侯龙史陶黎贺顾毛郝龚邵万钱严赖覃洪武莫孔汤明伟强芳英华敏静丽军勇杰磊秀玉平建国涛超鹏宇浩文斌红玲娟萍霞丹欣颖慧佳宁'
for line in bz2.decompress(source.read_bytes()).decode().splitlines():
    if not line or line.startswith('#'):continue
    fields=line.split('\t')
    if len(fields)!=3 or len(fields[0])!=1:continue
    ch,py,freq=fields
    try:ch.encode('gb2312');score=int(freq)
    except (UnicodeEncodeError,ValueError):continue
    for syllable in py.split():
        key=re.sub('[1-5]','',syllable).replace('ü','v').replace('u:','v')
        if not re.fullmatch('[a-z]{1,6}',key):continue
        words[key][ch]=score
out=''.join(key+'\t'+''.join(sorted(chars,key=lambda c:(c not in priority,-chars[c],ord(c))))+'\n' for key,chars in sorted(words.items()))
(root/'assets/ime/pinyin.tsv').write_text(out)
print(len(words),'syllables',len(set(''.join(''.join(v) for v in words.values()))),'characters')
