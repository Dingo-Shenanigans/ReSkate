"""Codec/identity checks independent of local game assets."""
import math
import struct
import unittest

import numpy as np

from decode_native_tree import category, identity, references
from native_piecewise import cached_dof_names, decode_quaternions, decode_vectors, sample_quaternions
from test_native_piecewise import vector_fixture


def quaternion_fixture(values, frames, wide=False):
    h=bytearray(80);struct.pack_into('<IQ',h,0,1,80)
    struct.pack_into('<4I',h,36,1,1,0,int(wide))
    times=np.asarray(frames,dtype='>u2' if wide else 'u1').tobytes()
    encoded=bytearray()
    for q in values:
        omitted=int(np.argmax(np.abs(q)))
        q=np.asarray(q,float)*(1 if q[omitted]>=0 else -1)
        u=np.clip(np.rint((q[np.arange(4)!=omitted]*math.sqrt(2)+1)*32768),0,65535).astype(np.uint16)
        u[0]=(int(u[0])&~1)|(omitted>>1);u[1]=(int(u[1])&~1)|(omitted&1)
        encoded.extend(u.astype('>u2').tobytes())
    data=times+encoded
    struct.pack_into('<IQ',h,56,len(data),96)
    return h+struct.pack('<III',len(values),0,len(times))+bytes(4)+data+bytes(16)


class NativeTreeTests(unittest.TestCase):
    def test_dynamic_quaternion_missing_component_and_wide_time(self):
        values=[]
        for i in range(4):
            q=np.array([.12,-.2,.08,.1]);q[i]=.95;q/=np.linalg.norm(q);values.append(q)
        track=decode_quaternions(quaternion_fixture(values,[0,1,256,400],True),['Hips.q'])['Hips.q']
        np.testing.assert_array_equal(track['frames'],[0,1,256,400])
        for q,actual in zip(values,track['values']):
            self.assertGreater(abs(np.dot(q,actual)),.99999999)

    def test_inline_native_constant_quaternion(self):
        h=bytearray(80);struct.pack_into('<IQ',h,0,1,80);struct.pack_into('<I',h,36,1);struct.pack_into('<IQ',h,56,0,96)
        blob=h+struct.pack('<I',1)+bytes.fromhex('7fec94100014417e')+bytes(4)+bytes(16)
        q=decode_quaternions(blob,['LeftToeBase.q'])['LeftToeBase.q']
        expected=[-.0096445517,.0000445071,-.2447527277,.9695375614]
        np.testing.assert_allclose(q['values'][0],expected,atol=1e-6)
        np.testing.assert_allclose(sample_quaternions(q,np.array([0,1,120])),np.tile(q['values'],(3,1)))

    def test_shortest_arc_and_outside_key_range(self):
        q=np.array([0,0,0,1.0]);b=np.array([0,math.sin(.5),0,math.cos(.5)])
        t={'frames':np.array([10,20]),'values':np.array([q,-b])}
        result=sample_quaternions(t,np.array([0,10,15,20,30]))
        np.testing.assert_allclose(result[0],q)
        self.assertGreater(abs(np.dot(result[2],[0,math.sin(.25),0,math.cos(.25)])),.9999999)
        self.assertGreater(abs(np.dot(result[-1],b)),.9999999)

    def test_quaternion_bounds_codec_and_key_order(self):
        fixture=quaternion_fixture([[0,0,0,1]]*3,[0,10,20])
        for at,value in [(36,99),(56,999999),(80,20001),(84,999999)]:
            blob=bytearray(fixture);struct.pack_into('<I',blob,at,value)
            with self.assertRaises(ValueError):decode_quaternions(blob,['Hips.q'])
        with self.assertRaises(ValueError):decode_quaternions(quaternion_fixture([[0,0,0,1]]*3,[0,0,20]),['Hips.q'])

    def test_measured_short_vectors_can_be_linear_and_scales_are_qualified(self):
        blob=bytearray(vector_fixture())
        offset=struct.unpack_from('<Q',blob,16)[0]
        struct.pack_into('<III',blob,offset,2,0,40)
        struct.pack_into('<3f',blob,offset+16,0,-.53938186,0)
        struct.pack_into('<3f',blob,offset+32,0,-.56922835,0)
        with self.assertRaises(ValueError):decode_vectors(blob,['Ground.t'])
        track=decode_vectors(blob,['Ground.t'],allow_linear_short=True)['Ground']
        self.assertAlmostEqual(float(np.interp(20,track['frames'],track['values'][:,1])),-.554305105,places=6)
        self.assertIn('Ground.s',decode_vectors(blob,['Ground.s'],qualified=True,allow_linear_short=True))

    def test_tree_identity_taxonomy_and_references(self):
        a='animation/dingo/c_proto_onb_grind_5050_rail_into'
        self.assertEqual(category(a,'ClipControllerAsset'),'On board/Grinds/5050/Entry')
        self.assertEqual(category('animation/dingo/x','BoolAsset'),'Graph/Parameters/Bool')
        self.assertNotEqual(identity('animation/dingo/a/clip'),identity('animation/dingo/b/clip'))
        self.assertNotIn('/',identity(a))
        rows=list(references({'fields':[{'kind':'external','partition':'guid','instance':'id'}, {'kind':'internal','instance':3}]}))
        self.assertEqual([p for p,_ in rows],['/fields/0','/fields/1'])
        self.assertEqual(rows[0][1]['partition'],'guid')

    def test_dynamic_64_bit_quaternions_are_little_endian(self):
        h=bytearray(80);struct.pack_into('<IQ',h,0,1,80);struct.pack_into('<4I',h,36,2,1,0,0)
        data=bytes([0,10,20])+bytes.fromhex('0300200000040080')*3
        struct.pack_into('<IQ',h,56,len(data),96)
        blob=h+struct.pack('<III',3,0,3)+bytes(4)+data+bytes(16)
        track=decode_quaternions(blob,['Hips.q'])['Hips.q']
        np.testing.assert_array_equal(track['values'],np.tile([0,0,0,1],(3,1)))

    def test_cached_rig_bindings_are_verified_from_native_layout(self):
        rig={'instances':[{'object':{'fields':dict(DofIds=[17],DofSetIdIndices=[0],DofSets=[dict(partition='layout')])}}]}
        blob=bytearray(72);struct.pack_into('<II',blob,16,64,7);blob[64:71]=b'Hips.q\0'
        document={'instances':[{'object':{'fields':{'LayoutData':list(struct.unpack('<9Q',blob))}}}]}
        self.assertEqual(cached_dof_names(rig,lambda _:document),{17:'Hips.q'})
        blob[16]=65
        bad={'instances':[{'object':{'fields':{'LayoutData':list(struct.unpack('<9Q',blob))}}}]}
        with self.assertRaises(ValueError):cached_dof_names(rig,lambda _:bad)


if __name__=='__main__':unittest.main()
