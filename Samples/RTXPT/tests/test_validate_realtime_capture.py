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
                             dynamic_motion_validated=True, denoiser='NRD_REBLUR',
                             sun_shadow_denoiser='SIGMA_SHADOW',
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

    def test_relax_without_sun_shadow_accepted(self):
        self.manifest.update(denoiser='NRD_RELAX', sun_shadow_denoiser=None)
        self.assertEqual(4, self.run_capture()['frame_index'])

    def test_unknown_denoiser_rejected(self):
        self.manifest['denoiser'] = 'NRD_SIGMA'
        with self.assertRaisesRegex(ValueError, 'denoisers'):
            self.run_capture()

    def test_unknown_sun_shadow_denoiser_rejected(self):
        self.manifest['sun_shadow_denoiser'] = 'SIGMA_SHADOW_TRANSLUCENCY'
        with self.assertRaisesRegex(ValueError, 'sun shadow denoiser'):
            self.run_capture()

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


class RRCaptureTest(unittest.TestCase):
    RENDER = 2
    DISPLAY = 4
    EXPOSURE = 0.15
    SKY_DEPTH = 5000000.0
    # The log2 exposure an auto exposure frame used, exact in 32-bit float, and its state texel holding it.
    AUTO_USED_EV = -2.0
    AUTO_STATE_TEXELS = 2
    AUTO_STATE_USED_EV = 7

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)
        self.manifest = dict(schema=2, mode='RealtimeDLSSRR', frame_index=4,
                             render_size=[self.RENDER, self.RENDER], display_size=[self.DISPLAY, self.DISPLAY],
                             spp=1, sample_base_index=3, jitter=[0.25, -0.125],
                             dynamic_motion_validated=True, denoiser=None, nrd_instances=0,
                             ray_reconstruction=dict(provider='NGX: DLSS RR', quality='Balanced', preset='F',
                                 executes=4, resets=1, failures=0, last_reset=False,
                                 exposure=struct.unpack('<f', struct.pack('<f', self.EXPOSURE))[0],
                                 view_forward_sign=-1, micro_jitter=0,
                                 depth_encoding='forward_positive_linear_view_depth_sky_5000000',
                                 motion_convention='current_to_previous_render_pixels_without_jitter',
                                 exposure_policy='explicit_exposure_texture_preexposure_1_scale_1'),
                             resources=[], layers=dict(file='layers.bin', bytes=0),
                             gpu_timing=dict(available=True, source_frame=4,
                                 milliseconds=dict(BUILD=1.0, FILL=1.0, Guides=0.1, RRPrepare=0.1, RR=0.5, ToneMapping=0.1)),
                             unavailable_stages=['NRD', 'SR', 'TAA', 'Bloom'])
        (self.directory / 'layers.bin').write_bytes(b'')
        texels = self.RENDER * self.RENDER
        self.write('rr_color', self.RENDER, 'RGBA16_FLOAT', 'e', [0.5] * 4 * texels)
        self.write('reconstructed_radiance', self.DISPLAY, 'RGBA16_FLOAT', 'e', [0.5] * 4 * self.DISPLAY * self.DISPLAY)
        self.write('display', self.DISPLAY, 'RGBA8_TYPELESS', 'B', [128] * 4 * self.DISPLAY * self.DISPLAY)
        self.write('rr_depth', self.RENDER, 'R32_FLOAT', 'f', [1.0, 2.5, self.SKY_DEPTH, 3.0])
        self.write('rr_motion', self.RENDER, 'RG16_FLOAT', 'e', [-0.25] * 2 * texels)
        self.write('rr_specular_motion', self.RENDER, 'RG16_FLOAT', 'e', [0.5] * 2 * texels)
        self.write('rr_normal_roughness', self.RENDER, 'RGBA16_FLOAT', 'e', [0.0, 0.6, 0.8, 0.5] * texels)
        self.write('rr_diffuse_albedo', self.RENDER, 'RGBA16_FLOAT', 'e', [0.5, 0.25, 0.0, 1.0] * texels)
        self.write('rr_specular_albedo', self.RENDER, 'RGBA16_FLOAT', 'e', [0.04, 0.04, 1.0, 1.0] * texels)
        self.write('rr_exposure', 1, 'R32_FLOAT', 'f', [self.EXPOSURE])

    def write(self, name, size, fmt, code, values, height=None):
        height = size if height is None else height
        data = struct.pack('<' + code * len(values), *values)
        (self.directory / (name + '.bin')).write_bytes(data)
        resources = [r for r in self.manifest['resources'] if r['name'] != name]
        resources.append(dict(name=name, file=name + '.bin', format='TEX_FORMAT_' + fmt, width=size, height=height,
                              slices=1, row_bytes=len(data) // height, bytes=len(data), valid=True, frame_index=4))
        self.manifest['resources'] = resources

    def write_auto_state(self, used_ev):
        state = [0.0] * (4 * self.AUTO_STATE_TEXELS)
        state[self.AUTO_STATE_USED_EV] = used_ev
        self.write('auto_exposure_next', self.AUTO_STATE_TEXELS, 'RGBA32_FLOAT', 'f', state, height=1)

    def make_auto_exposure(self):
        used = 2.0 ** self.AUTO_USED_EV
        self.manifest.update(exposure_mode='Auto', auto_exposure=dict(used='auto_exposure_used', next='auto_exposure_next'))
        self.manifest['ray_reconstruction'].update(exposure=None, exposure_source='auto_exposure_used')
        self.write('auto_exposure_used', 1, 'R32_FLOAT', 'f', [used])
        self.write_auto_state(self.AUTO_USED_EV)
        self.write('rr_exposure', 1, 'R32_FLOAT', 'f', [used])
        return used

    def run_capture(self):
        (self.directory / 'manifest.json').write_text(json.dumps(self.manifest))
        return validate(self.directory)

    def test_valid_capture(self):
        self.assertEqual(4, self.run_capture()['frame_index'])
        self.assertTrue((self.directory / 'reconstructed_radiance.pfm').exists())

    def write_bloom(self, size):
        self.write('bloom_radiance', size, 'RGBA16_FLOAT', 'e', [0.5] * 4 * size * size)

    def test_valid_bloom_capture(self):
        self.write_bloom(self.DISPLAY)
        self.manifest['unavailable_stages'] = ['NRD', 'SR', 'TAA']
        self.assertEqual(4, self.run_capture()['frame_index'])

    def test_bloom_radiance_with_unavailable_bloom_rejected(self):
        self.write_bloom(self.DISPLAY)
        with self.assertRaisesRegex(ValueError, 'Bloom radiance'):
            self.run_capture()

    def test_available_bloom_without_radiance_rejected(self):
        self.manifest['unavailable_stages'] = ['NRD', 'SR', 'TAA']
        with self.assertRaisesRegex(ValueError, 'Bloom radiance'):
            self.run_capture()

    def test_render_size_bloom_radiance_rejected(self):
        self.write_bloom(self.RENDER)
        self.manifest['unavailable_stages'] = ['NRD', 'SR', 'TAA']
        with self.assertRaisesRegex(ValueError, 'Bloom extent'):
            self.run_capture()

    def test_missing_rr_input_rejected(self):
        self.manifest['resources'] = [r for r in self.manifest['resources'] if r['name'] != 'rr_specular_albedo']
        with self.assertRaisesRegex(ValueError, 'Missing RR'):
            self.run_capture()

    def test_render_size_reconstruction_rejected(self):
        self.write('reconstructed_radiance', self.RENDER, 'RGBA16_FLOAT', 'e', [0.5] * 4 * self.RENDER * self.RENDER)
        with self.assertRaisesRegex(ValueError, 'extent'):
            self.run_capture()

    def test_exposure_texture_not_one_texel_rejected(self):
        self.write('rr_exposure', self.RENDER, 'R32_FLOAT', 'f', [self.EXPOSURE] * self.RENDER * self.RENDER)
        with self.assertRaisesRegex(ValueError, 'extent'):
            self.run_capture()

    def test_valid_auto_exposure_capture(self):
        self.make_auto_exposure()
        self.assertEqual(4, self.run_capture()['frame_index'])

    def test_auto_exposure_state_of_another_frame_rejected(self):
        self.make_auto_exposure()
        self.write_auto_state(self.AUTO_USED_EV + 2)
        with self.assertRaisesRegex(ValueError, 'another frame'):
            self.run_capture()

    def test_rr_exposure_other_than_auto_exposure_rejected(self):
        used = self.make_auto_exposure()
        self.write('rr_exposure', 1, 'R32_FLOAT', 'f', [used * 4])
        with self.assertRaisesRegex(ValueError, 'RR read another exposure'):
            self.run_capture()

    def test_reported_rr_exposure_with_auto_exposure_rejected(self):
        used = self.make_auto_exposure()
        self.manifest['ray_reconstruction']['exposure'] = used
        with self.assertRaisesRegex(ValueError, 'RR input semantics'):
            self.run_capture()

    def test_missing_auto_exposure_state_rejected(self):
        self.make_auto_exposure()
        self.manifest['resources'] = [r for r in self.manifest['resources'] if r['name'] != 'auto_exposure_next']
        with self.assertRaisesRegex(ValueError, 'Auto exposure resources'):
            self.run_capture()

    def test_auto_exposure_resource_in_manual_capture_rejected(self):
        self.write('auto_exposure_used', 1, 'R32_FLOAT', 'f', [self.EXPOSURE])
        with self.assertRaisesRegex(ValueError, 'Manual exposure capture'):
            self.run_capture()

    def test_auto_exposure_state_extent_rejected(self):
        self.make_auto_exposure()
        self.write('auto_exposure_next', 1, 'RGBA32_FLOAT', 'f', [self.AUTO_USED_EV] * 4)
        with self.assertRaisesRegex(ValueError, 'auto exposure layout'):
            self.run_capture()

    def test_exposure_other_than_reported_rejected(self):
        self.write('rr_exposure', 1, 'R32_FLOAT', 'f', [1.0])
        with self.assertRaisesRegex(ValueError, 'exposure'):
            self.run_capture()

    def test_nonpositive_depth_rejected(self):
        self.write('rr_depth', self.RENDER, 'R32_FLOAT', 'f', [1.0, -2.5, self.SKY_DEPTH, 3.0])
        with self.assertRaisesRegex(ValueError, 'depth'):
            self.run_capture()

    def test_depth_beyond_sky_rejected(self):
        self.write('rr_depth', self.RENDER, 'R32_FLOAT', 'f', [1.0, 2.5, 2 * self.SKY_DEPTH, 3.0])
        with self.assertRaisesRegex(ValueError, 'depth'):
            self.run_capture()

    def test_non_unit_normal_rejected(self):
        self.write('rr_normal_roughness', self.RENDER, 'RGBA16_FLOAT', 'e', [0.0, 0.6, 0.6, 0.5] * self.RENDER * self.RENDER)
        with self.assertRaisesRegex(ValueError, 'normal'):
            self.run_capture()

    def test_roughness_outside_unit_range_rejected(self):
        self.write('rr_normal_roughness', self.RENDER, 'RGBA16_FLOAT', 'e', [0.0, 0.6, 0.8, 1.5] * self.RENDER * self.RENDER)
        with self.assertRaisesRegex(ValueError, 'roughness'):
            self.run_capture()

    def test_albedo_outside_unit_range_rejected(self):
        self.write('rr_diffuse_albedo', self.RENDER, 'RGBA16_FLOAT', 'e', [1.5, 0.25, 0.0, 1.0] * self.RENDER * self.RENDER)
        with self.assertRaisesRegex(ValueError, 'albedo'):
            self.run_capture()

    def test_nonfinite_input_rejected(self):
        self.write('rr_color', self.RENDER, 'RGBA16_FLOAT', 'e', [float('inf')] * 4 * self.RENDER * self.RENDER)
        with self.assertRaisesRegex(ValueError, 'Nonfinite'):
            self.run_capture()

    def test_nrd_output_rejected(self):
        self.write('history_confidence_layer0', self.RENDER, 'R8_UNORM', 'B', [255] * self.RENDER * self.RENDER)
        with self.assertRaisesRegex(ValueError, 'NRD'):
            self.run_capture()

    def test_nrd_instance_rejected(self):
        self.manifest['nrd_instances'] = 1
        with self.assertRaisesRegex(ValueError, 'NRD'):
            self.run_capture()

    def test_sun_shadow_denoiser_rejected(self):
        self.manifest['sun_shadow_denoiser'] = 'SIGMA_SHADOW'
        with self.assertRaisesRegex(ValueError, 'sun shadow denoiser'):
            self.run_capture()

    def test_rr_reported_unavailable_rejected(self):
        self.manifest['unavailable_stages'] = ['NRD', 'SR', 'TAA', 'RR', 'Bloom']
        with self.assertRaisesRegex(ValueError, 'stage'):
            self.run_capture()

    def test_micro_jitter_rejected(self):
        self.manifest['ray_reconstruction']['micro_jitter'] = 0.1
        with self.assertRaisesRegex(ValueError, 'RR input semantics'):
            self.run_capture()

    def test_unknown_forward_sign_rejected(self):
        self.manifest['ray_reconstruction']['view_forward_sign'] = 0
        with self.assertRaisesRegex(ValueError, 'RR input semantics'):
            self.run_capture()

    def test_unknown_quality_rejected(self):
        self.manifest['ray_reconstruction']['quality'] = 'UltraPerformance'
        with self.assertRaisesRegex(ValueError, 'RR provider'):
            self.run_capture()

    def test_missing_rr_timing_rejected(self):
        self.manifest['gpu_timing']['milliseconds']['RRPrepare'] = None
        with self.assertRaisesRegex(ValueError, 'timing'):
            self.run_capture()

    def test_wrong_frame_jitter_metadata_rejected(self):
        self.manifest['sample_base_index'] = 4
        with self.assertRaisesRegex(ValueError, 'frame'):
            self.run_capture()


if __name__ == '__main__':
    unittest.main()
