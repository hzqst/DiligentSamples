import json
from pathlib import Path
import struct
import tempfile
import unittest

from validate_realtime_capture import validate


class SRCaptureTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)
        self.manifest = dict(schema=2, mode='RealtimeDLSSSR', frame_index=4,
                             render_size=[2, 2], display_size=[4, 4], spp=1,
                             sample_base_index=3, jitter=[0.25, -0.125],
                             dynamic_motion_validated=True, denoiser='NRD_RELAX',
                             super_resolution=dict(provider='NGX: DLSS', quality='Quality',
                                 executes=4, resets=1, failures=0,
                                 depth_encoding='forward_projection_0_1_far_1',
                                 motion_convention='current_to_previous_render_pixels_without_jitter',
                                 exposure_policy='explicit_1_preexposure_1_scale_1'),
                             resources=[], layers=dict(file='layers.bin', bytes=0),
                             gpu_timing=dict(available=False))
        (self.directory / 'layers.bin').write_bytes(b'')
        for name, size, fmt, channels, code, value in [
            ('denoised_radiance', 2, 'RGBA16_FLOAT', 4, 'e', 0.5),
            ('upscaled_radiance', 4, 'RGBA16_FLOAT', 4, 'e', 0.5),
            ('display', 4, 'RGBA8_TYPELESS', 4, 'B', 128),
            ('sr_depth', 2, 'R32_FLOAT', 1, 'f', 0.5),
            ('sr_motion', 2, 'RG16_FLOAT', 2, 'e', -0.25),
            ('sr_ignore_history', 2, 'R8_UNORM', 1, 'B', 255),
            *[(f'history_confidence_layer{i}', 2, 'R8_UNORM', 1, 'B', 255) for i in range(3)],
        ]:
            data = struct.pack('<' + code, value) * (size * size * channels)
            (self.directory / (name + '.bin')).write_bytes(data)
            self.manifest['resources'].append(dict(name=name, file=name + '.bin',
                format='TEX_FORMAT_' + fmt, width=size, height=size, slices=1,
                row_bytes=len(data) // size, bytes=len(data), valid=True, frame_index=4))

    def run_capture(self):
        (self.directory / 'manifest.json').write_text(json.dumps(self.manifest))
        return validate(self.directory)

    def test_valid_dual_resolution_capture(self):
        self.assertEqual(4, self.run_capture()['frame_index'])
        self.assertTrue((self.directory / 'upscaled_radiance.pfm').exists())

    def test_missing_sr_input_rejected(self):
        self.manifest['resources'] = [r for r in self.manifest['resources'] if r['name'] != 'sr_motion']
        with self.assertRaisesRegex(ValueError, 'Missing SR'):
            self.run_capture()

    def test_wrong_display_extent_rejected(self):
        self.manifest['display_size'] = [2, 2]
        with self.assertRaisesRegex(ValueError, 'extent'):
            self.run_capture()

    def test_depth_outside_projection_range_rejected(self):
        (self.directory / 'sr_depth.bin').write_bytes(struct.pack('<4f', 0, 0.5, 1, 1.25))
        with self.assertRaisesRegex(ValueError, 'depth'):
            self.run_capture()

    def test_nonfinite_motion_rejected(self):
        (self.directory / 'sr_motion.bin').write_bytes(struct.pack('<8e', *([float('nan')] * 8)))
        with self.assertRaisesRegex(ValueError, 'Nonfinite'):
            self.run_capture()

    def test_wrong_frame_jitter_metadata_rejected(self):
        self.manifest['sample_base_index'] = 4
        with self.assertRaisesRegex(ValueError, 'frame'):
            self.run_capture()

    def test_invalid_format_rejected(self):
        next(r for r in self.manifest['resources'] if r['name'] == 'sr_motion')['format'] = 'TEX_FORMAT_RGBA16_FLOAT'
        with self.assertRaisesRegex(ValueError, 'format'):
            self.run_capture()

    def test_resource_from_another_frame_rejected(self):
        self.manifest['resources'][0]['frame_index'] = 3
        with self.assertRaisesRegex(ValueError, 'another frame'):
            self.run_capture()


if __name__ == '__main__':
    unittest.main()
