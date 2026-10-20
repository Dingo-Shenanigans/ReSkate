"""Offline regression checks for sparse native clip packaging. Run with the bundled Python."""
import copy
import struct
import unittest

from native_clip_pipeline import sparse_keys, raw_cas, controller_contract
from native_resource_marker import mark, TARGETS


class NativeClipPipelineTests(unittest.TestCase):
    def marker_blob(self):
        names=list(TARGETS)+['Spine.q']
        header=bytearray(80);struct.pack_into('<IQ',header,0,len(names),80)
        struct.pack_into('<I',header,36,1)
        data=bytearray();descriptors=bytearray()
        for i,_ in enumerate(names):
            times=len(data);data+=bytes((0,1));values=len(data);data+=bytes((i+1,))*12
            descriptors+=struct.pack('<3I',2,times,values)
        start=80+len(descriptors)
        struct.pack_into('<IQ',header,56,len(data),start)
        return bytes(header+descriptors+data)+struct.pack('<4I',4,16,28,60),list(enumerate(names))

    def test_marker_changes_only_owned_arm_values(self):
        blob,rows=self.marker_blob();result,report=mark(blob,rows,bytes.fromhex('8001ffff8000'))
        self.assertEqual(blob[:140],result[:140])
        self.assertEqual(blob[-30:],result[-30:]) # Untouched spine curve and relocations.
        self.assertEqual(len(report['curves']),4)
        self.assertEqual(len(blob),len(result))

    def test_marker_rejects_shared_data_or_unknown_codec(self):
        blob,rows=self.marker_blob()
        bad=bytearray(blob);struct.pack_into('<I',bad,36,3)
        with self.assertRaises(ValueError):mark(bad,rows,bytes(6))
        bad=bytearray(blob);struct.pack_into('<I',bad,88,58) # Arm aliases the spine value range.
        with self.assertRaises(ValueError):mark(bad,rows,bytes(6))

    def blob(self):
        # One quaternion, two vectors (only the first authored), one scalar.
        header = bytearray(80)
        struct.pack_into('<IQIQIQ',header,0,1,80,2,96,1,192)
        struct.pack_into('<4I',header,36,3,2,2,1)
        data = bytes(range(128))
        struct.pack_into('<IQ',header,56,len(data),224)
        q = struct.pack('<3I',1,0,2)
        v1 = struct.pack('<3I',1,18,20) + bytes(36)
        v2 = struct.pack('<3I',1,32,34) + bytes(36)
        f = struct.pack('<3I',1,46,48) + bytes(8)
        return bytes(header)+q+bytes(4)+v1+v2+f+bytes(12)+data+struct.pack('<4I',4,16,28,60)

    def test_filter_keeps_exact_curve_descriptors_and_shared_data(self):
        rows = [(1,'Hips.q'),(2,'Hips.t'),(3,'Hips.s'),(4,'LeftFoot.onboard')]
        result = sparse_keys(self.blob(),rows,[rows[i] for i in (0,1,3)])
        self.assertEqual([struct.unpack_from('<I',result,o)[0] for o in (0,12,24)],[1,1,1])
        self.assertEqual(result[-144:],self.blob()[-144:])
        v = struct.unpack_from('<Q',result,16)[0]
        self.assertEqual(result[v:v+48],self.blob()[96:144])
        f = struct.unpack_from('<Q',result,28)[0]
        self.assertEqual(result[f:f+20],self.blob()[192:212])

    def test_aligned_relocation_padding_is_preserved_and_validated(self):
        blob=bytearray(self.blob())
        struct.pack_into('<I',blob,56,120)
        blob[-24:-16]=bytes(8)
        rows=[(1,'Hips.q'),(2,'Hips.t'),(3,'Hips.s'),(4,'LeftFoot.onboard')]
        result=sparse_keys(bytes(blob),rows,rows)
        self.assertEqual(result[-144:],blob[-144:])
        blob[-17]=1
        with self.assertRaises(ValueError):sparse_keys(bytes(blob),rows,rows)

    def test_unknown_codec_and_broken_bounds_fail(self):
        rows = [(1,'Hips.q'),(2,'Hips.t'),(3,'Hips.s'),(4,'LeftFoot.onboard')]
        for offset,value in ((36,99),(56,999999),(80,999999)):
            broken = bytearray(self.blob());struct.pack_into('<I',broken,offset,value)
            with self.assertRaises(ValueError):
                sparse_keys(broken,rows,rows)

    def test_raw_cas_crosses_64k_without_truncation(self):
        data = bytes(range(256))*300
        encoded = raw_cas(data)
        out = bytearray();at=0
        while at<len(encoded):
            header = struct.unpack_from('>Q',encoded,at)[0];at+=8
            size=header & 0xfffff
            self.assertEqual((header>>32)&0xffffff,size)
            self.assertEqual((header>>24)&0x7f,0)
            out+=encoded[at:at+size];at+=size
        self.assertEqual(out,data)

    def test_native_tag_changes_are_rejected(self):
        before = dict(partition='id',root_type='ClipControllerAsset',instances=[dict(guid='id',index=0,
            object=dict(type='EclipseAnimationAsset',fields=dict(Resource=1,ChannelToDofAsset=2,EndFrame=121,FloatChannelSize=6)))])
        after = copy.deepcopy(before)
        after['instances'][0]['object']['fields'].update(Resource=3,ChannelToDofAsset=4)
        self.assertEqual(controller_contract(before,after),1)
        for key in ('EndFrame','FloatChannelSize'):
            changed=copy.deepcopy(after);changed['instances'][0]['object']['fields'][key]+=1
            with self.assertRaises(ValueError):controller_contract(before,changed)


if __name__ == '__main__':
    unittest.main()
