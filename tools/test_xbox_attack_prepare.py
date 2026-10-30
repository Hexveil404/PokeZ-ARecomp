"""Synthetic preparation/rollback tests; all private outputs stay in temporary folders."""
import importlib.util
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools/za'))
import xbox_attack_layout as x


def synthetic_archive():
    sections = []
    for i, (xy, template) in enumerate(zip([(0,46),(54,-9),(-54,-9),(0,-62)], [1,0,2,1])):
        extra = f'btl_app_waza_parts_{template:02d}'.encode()+b'\0'
        size = (84+len(extra)+3)&~3
        pane = bytearray(size)
        struct.pack_into('<4sI', pane, 0, b'prt1', size)
        name = f'L_waza_{i:02d}'.encode()
        pane[12:12+len(name)] = name
        struct.pack_into('<2f', pane, 44, *xy)
        pane[84:84+len(extra)] = extra
        sections.append(pane)
    flyt = bytearray(20)+b''.join(sections)
    struct.pack_into('<4sHHIIHH', flyt, 0, b'FLYT', 0xfeff, 20, 0x09000000, len(flyt), 4, 0)
    members = [(x.MEMBER, bytes(flyt)), ('anim/synthetic.bflan', b'FLAN synthetic N_inout')]
    names = bytearray();nodes = bytearray();payload = bytearray()
    for name, content in members:
        no = len(names)//4
        names += name.encode()+b'\0'
        names += bytes((-len(names))%4)
        start = len(payload);payload += content
        nodes += struct.pack('<4I',0,0x1000000|no,start,len(payload))
    header = bytearray(20)
    table = struct.pack('<4sHHI',b'SFAT',12,len(members),0x65)+nodes
    table += struct.pack('<4sHH',b'SFNT',8,0)+names
    start = len(header)+len(table)
    data = header+table+payload
    struct.pack_into('<4sHHIIHH',data,0,b'SARC',20,0xfeff,len(data),start,0x100,0)
    return bytes(data)


class Preparation(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='xbox-prepare-')
        self.root = Path(self.tmp.name)
        self.mods = self.root/'mods'
        self.arc = synthetic_archive()
        self.guard = {'offset':32,'size':16,'fnv64':f'{x.fnv(bytes(range(16))):016x}'}

    def tearDown(self):
        self.tmp.cleanup()

    def install(self, stamp='4-xbox1-test', data=None, guard=None):
        return x.install(data or self.arc, guard or self.guard, self.mods, stamp)

    def snapshot(self):
        return {p.relative_to(self.mods).as_posix():p.read_bytes() for p in self.mods.rglob('*') if p.is_file()}

    def test_schema_and_no_source_header(self):
        self.install()
        target = self.mods/x.MOD_NAME
        blob = (target/'resource.bin').read_bytes()
        fields = struct.unpack('<8s9Q', blob)
        self.assertEqual(fields[:6], (x.MAGIC,len(self.arc),x.fnv(self.arc),32,16,int(self.guard['fnv64'],16)))
        self.assertEqual(list(fields[6:]),x.inspect(self.arc)['digits'])
        self.assertFalse(list(self.mods.rglob('*.h')))
        self.assertEqual((target/'romfs'/x.VIRTUAL).read_bytes(),self.arc)

    def test_repeat_does_not_replace_archive(self):
        self.install();path=self.mods/x.MOD_NAME/'romfs'/x.VIRTUAL
        mtime=path.stat().st_mtime_ns
        self.assertEqual(self.install('new-stamp'),'already prepared')
        self.assertEqual(path.stat().st_mtime_ns,mtime)
        self.assertEqual((self.mods/x.STATUS).read_text(),'new-stamp 1\n')

    def test_corrupt_output_is_repaired(self):
        self.install();path=self.mods/x.MOD_NAME/'romfs'/x.VIRTUAL
        bad=bytearray(self.arc);bad[-1]^=1;path.write_bytes(bad)
        self.install();self.assertEqual(path.read_bytes(),self.arc)

    def test_update_does_not_require_compilation(self):
        self.install()
        arc=self.arc[:-1]+bytes([self.arc[-1]^1])
        guard=dict(self.guard,offset=4096)
        self.install('updated',arc,guard)
        fields=struct.unpack('<8s9Q',(self.mods/x.MOD_NAME/'resource.bin').read_bytes())
        self.assertEqual(fields[2],x.fnv(arc));self.assertEqual(fields[3],4096)

    def test_reject_geometry_animation_and_missing_pane(self):
        self.install();before=self.snapshot()
        info=x.inspect(self.arc);at=next(off for name,off,_ in x.members(self.arc) if name==x.MEMBER)
        pane=x.inspect(self.arc)['digits'][0]-at-20
        mutations=[]
        bad=bytearray(self.arc);struct.pack_into('<f',bad,at+pane+44,999);mutations.append(bad)
        bad=bytearray(self.arc);bad[info['digits'][1]]=ord('0');mutations.append(bad)
        bad=bytearray(self.arc);bad[-20:]=b'L_waza_00'+bytes(11);mutations.append(bad)
        for data in mutations+[self.arc[:16]]:
            with self.assertRaises((ValueError,struct.error)):
                self.install(data=bytes(data))
            self.assertEqual(self.snapshot(),before)

    def test_unsupported_source_preserves_old_and_records_reason(self):
        self.install();target=self.mods/x.MOD_NAME
        before={p.relative_to(target):p.read_bytes() for p in target.rglob('*') if p.is_file()}
        with patch.object(x,'extract',side_effect=ValueError('unsupported version')):
            self.assertFalse(x.prepare('unused',self.mods,'changed-source',{}))
        self.assertEqual((self.mods/x.STATUS).read_text(),'changed-source 0\n')
        self.assertEqual({p.relative_to(target):p.read_bytes() for p in target.rglob('*') if p.is_file()},before)

    def test_failed_publish_rolls_back(self):
        self.install();before=self.snapshot();replace=os.replace;target=self.mods/x.MOD_NAME
        def fail(src,dst):
            if Path(dst)==target and not str(src).endswith('-old'):raise OSError('injected publish failure')
            return replace(src,dst)
        with patch.object(x.os,'replace',side_effect=fail):
            with self.assertRaises(OSError):self.install('next',self.arc[:-1]+b'!')
        self.assertEqual(self.snapshot(),before)
        self.assertFalse(list(self.mods.glob('.XboxAttackLayout-*-old')))

    def test_failed_rollback_preserves_backup(self):
        self.install();replace=os.replace;target=self.mods/x.MOD_NAME
        def fail(src,dst):
            if Path(dst)==target:raise OSError('injected move/rollback failure')
            return replace(src,dst)
        with patch.object(x.os,'replace',side_effect=fail):
            with self.assertRaises(OSError):self.install('next',self.arc[:-1]+b'!')
        old=list(self.mods.glob('.XboxAttackLayout-*-old'))
        self.assertEqual(len(old),1)
        self.assertEqual((old[0]/'romfs'/x.VIRTUAL).read_bytes(),self.arc)

    def test_invalid_metadata_does_not_write(self):
        for guard in [dict(self.guard,size=x.MAX_PACK+1),dict(self.guard,offset=(1<<64)-1)]:
            with self.assertRaises(ValueError):self.install(guard=guard)
        self.assertFalse(self.mods.exists())

    def test_invalid_stamp_does_not_write(self):
        for stamp in ['', 'x 1\ninjected', 'x'*48]:
            with self.assertRaisesRegex(ValueError,'stamp'):self.install(stamp)
        self.assertFalse(self.mods.exists())

    def test_io_error_is_retryable(self):
        with patch.object(x,'extract',side_effect=OSError('temporary read failure')):
            self.assertFalse(x.prepare('unused',self.mods,'source-stamp',{}))
        self.assertFalse((self.mods/x.STATUS).exists())

    def test_target_link_is_rejected(self):
        original=Path.is_symlink
        with patch.object(Path,'is_symlink',lambda p:p.name==x.MOD_NAME or original(p)):
            with self.assertRaisesRegex(ValueError,'link'):self.install()
        self.assertFalse(self.mods.exists())


if __name__=='__main__':unittest.main()
