"""Independent known-value fixtures for the measured native position format."""
import struct
import unittest
import numpy as np

from native_piecewise import decode_vectors, decode_scalars, correct_translations
from repair_library_positions import read_clip, encode_clip
import tempfile
from pathlib import Path


def vector_fixture(wide=False, constant=False):
    header=bytearray(80); descriptor=bytearray(48)
    struct.pack_into('<IQ',header,12,1,80)
    struct.pack_into('<I',header,40,1);struct.pack_into('<I',header,48,int(wide))
    if constant:
        struct.pack_into('<III',descriptor,0,2,0,510)
        struct.pack_into('<3f',descriptor,16,-2,3,.5)
        struct.pack_into('<3f',descriptor,32,-2,3,.5)
        data=b''
    else:
        times=struct.pack('>3H',0,256,510) if wide else bytes((0,10,20))
        values=struct.pack('>9H',0,0,0,32768,32768,32768,65535,65535,65535)
        struct.pack_into('<III',descriptor,0,3,0,len(times))
        struct.pack_into('<3f',descriptor,16,-2,3,.5)
        struct.pack_into('<3f',descriptor,32,4,6,2)
        data=times+values
    struct.pack_into('<IQ',header,56,len(data),128)
    return header+descriptor+data+bytes(16)


class NativePiecewiseTests(unittest.TestCase):
    def test_nonzero_minimum_uses_extent_not_maximum(self):
        values=decode_vectors(vector_fixture(),['Hips.t'])['Hips']
        np.testing.assert_allclose(values['values'][0],[-2,3,.5])
        np.testing.assert_allclose(values['values'][-1],[2,9,2.5])
        np.testing.assert_allclose(values['legacy_values'][-1],[4,6,2])
        np.testing.assert_allclose(values['values'][1],[-2+32768/65535*4,3+32768/65535*6,.5+32768/65535*2])

    def test_big_endian_wide_times_and_inline_constant(self):
        np.testing.assert_array_equal(decode_vectors(vector_fixture(True),['Ground.t'])['Ground']['frames'],[0,256,510])
        v=decode_vectors(vector_fixture(constant=True),['Ground.t'])['Ground']
        np.testing.assert_array_equal(v['values'],[[-2,3,.5],[-2,3,.5]])

    def test_bad_bounds_and_short_encodings_are_refused(self):
        blob=vector_fixture();struct.pack_into('<I',blob,88,0xffffffff)
        with self.assertRaises(ValueError):decode_vectors(blob,['Ground.t'])
        blob=vector_fixture(constant=True);struct.pack_into('<f',blob,112,5)
        with self.assertRaises(ValueError):decode_vectors(blob,['Ground.t'])

    def test_exporter_convention_is_checked_before_replacement(self):
        track=decode_vectors(vector_fixture(),['Hips.t'])
        class Clip:
            node={'Hips':None};times=np.array([0,10,20])/60;start=0
            def channel(self,*args):return self.values.copy()
            def set_channel(self,*args):self.values=args[-1]
        clip=Clip();clip.values=track['Hips']['legacy_values'].copy()
        report=correct_translations(clip,track)
        self.assertEqual(report[0]['exporter'],'extent_as_maximum')
        np.testing.assert_allclose(clip.values,track['Hips']['values'])
        clip.values+=100
        with self.assertRaises(ValueError):correct_translations(clip,track)

    def test_quantized_scalar_extent_and_inline_values(self):
        h=bytearray(80);struct.pack_into('<IQ',h,24,1,80)
        data=bytes((0,10,20,0,128,255));struct.pack_into('<IQ',h,56,len(data),100)
        d=struct.pack('<IIIff',3,0,3,2,6)
        track=decode_scalars(h+d+data+bytes(16),['speed'])['speed']
        np.testing.assert_allclose(track['values'],[2,2+128/255*6,8])
        d=struct.pack('<IIIff',2,0,120,1.25,1.25);struct.pack_into('<IQ',h,56,0,100)
        np.testing.assert_allclose(decode_scalars(h+d+bytes(16),['LandingY'])['LandingY']['values'],[1.25,1.25])

    def test_staged_clip_codec_preserves_every_lane(self):
        times=np.array([0,.1,.2],dtype='<f4');frames=np.zeros((3,395,10),dtype='<f4')
        frames[:,:,:3]=1;frames[:,:,6]=1
        frames[:,380,8]=[.1,.5,.2];frames[:,376,8]=[.2,.8,.3]
        boards=frames[:,380].copy()
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'test.clip';path.write_bytes(encode_clip(times,frames,boards,.05,.15))
            t,f,b,up,down=read_clip(path)
            np.testing.assert_array_equal(t,times);np.testing.assert_array_equal(f,frames);np.testing.assert_array_equal(b,boards)
            self.assertAlmostEqual(up,.05);self.assertAlmostEqual(down,.15)


if __name__=='__main__':unittest.main()
