from configparser import ConfigParser, UNNAMED_SECTION
import os
import sys
import os.path as path
import json
import math
import numpy as np
import re

MAX_NUM_LIGHTS = 10
MAX_NUM_CAMERAS = 10
MAX_NUM_DDGI_VOLUMES = 10

class Quaternion:
    w: float
    x: float
    y: float
    z: float

    def __init__(self, w = 1.0, x = 0.0, y = 0.0, z = 1.0):
        self.w = w
        self.x = x
        self.y = y
        self.z = z

    def __mul__(self, b):
        a = self
        return Quaternion(
				a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
				a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
				a.w*b.y + a.y*b.w + a.z*b.x - a.x*b.z,
				a.w*b.z + a.z*b.w + a.x*b.y - a.y*b.x
        )
    
    @staticmethod
    def from_two_vectors(a: list, b: list):
        v1 = np.array(a)
        v2 = np.array(b)
        v1_l = np.sqrt(np.dot(v1, v1))
        v2_l = np.sqrt(np.dot(v2, v2))
        v3 = np.append(np.array([v1_l * v2_l + np.dot(v1, v2)]), np.cross(v1, v2))
        v3 = v3 / np.sqrt(np.dot(v3, v3))
        return Quaternion(
            v3[0],
            v3[1],
            v3[2],
            v3[3]
        )
    
    @staticmethod
    def from_euler_xyz(euler_angles: list):
        # RHS to LHS
        half_radians = -np.radians(0.5 * np.array(euler_angles))
        sin_half_radians = np.sin(half_radians)
        cos_half_radians = np.cos(half_radians)

        quat_x = Quaternion(cos_half_radians[0], sin_half_radians[0], 0.0, 0.0)
        quat_y = Quaternion(cos_half_radians[1], 0.0, sin_half_radians[1], 0.0)
        quat_z = Quaternion(cos_half_radians[2], 0.0, 0.0, sin_half_radians[2])
        return quat_z * quat_y * quat_x

    def tolist(self):
        return [self.x, self.y, self.z, self.w]
    
    def __str__(self):
        return f'{{ {self.x}, {self.y}, {self.z}, {self.w} }}'

def rf(s: str):
    m = re.match(r'^[\+\-]?\d*\.?\d+(?:[eE][\+\-]?\d+)?(?:f?)?', s)
    if m:
        return float(m.group(0))
    else:
        raise ValueError(s)

def parse_float_vector(s: str):
    return [rf(x) for x in s.split(' ')]

def parse_int_vector(s: str):
    return [int(x) for x in s.split(' ')]

def parse_ddgi_format(sfmt):

    ifmt = int(sfmt[0])

    if ifmt == 0:
        return 'R10G10B10A2_UNORM'
    elif ifmt == 1:
        return 'R16_FLOAT'
    elif ifmt == 2:
        return 'RG16_FLOAT'
    elif ifmt == 3:
        return 'RGBA16_FLOAT'
    elif ifmt == 4:
        return 'R32_FLOAT'
    elif ifmt == 5:
        return 'RG32_FLOAT'
    elif ifmt == 6:
        return 'RGBA32_FLOAT'
    else:
        return 'Unknown'

def parse_ddgi_vis_type(itype: int):
    if itype == 0:
        return 'Default'
    elif itype == 1:
        return 'Hide_Inactive'
    else:
        return 'Unknown'
    
def parse_render_mode(itype: int):
    if itype == 0:
        return "PATH_TRACING"
    else:
        return "DDGI"

def light_dir_to_quaternion(dir: list):
    return Quaternion.from_two_vectors([0.0, 0.0, -1.0], dir).tolist()


def default_scene_strm():
    return {
        "models": [],
        "graph": [
            {
                "name": "Lights",
                "children": []
            },
            {
                "name": "Cameras",
                "children": []
            }
        ]
    }

def cvt_model(config: ConfigParser, strm):
    model_path = path.join(config.get(UNNAMED_SECTION, "scene.path"),
                           config.get(UNNAMED_SECTION, "scene.file"))
    strm["models"] = [
        '../' + model_path
    ]
    graph = strm['graph']
    graph.append(
        {
            "name": config.get(UNNAMED_SECTION, "scene.name"),
            "model": 0
        }
    )

    # lights = next((x for x in graph if x['name'] == 'Lights'), None)
    # lights_children : list = lights['children']
    # lights_children.append(
    #     {
    #         "name": "SkyEnvironmentLight",
    #         "type": "EnvironmentLight",
    #         "color": parse_float_vector(config.get(UNNAMED_SECTION, 'scene.skyColor')),
    #         "intensity": config.getfloat(UNNAMED_SECTION, 'scene.skyIntensity')
    #     }
    # )

def cvt_scene_lights(config: ConfigParser, strm):
    lights = next((x for x in strm['graph'] if x['name'] == 'Lights'), None)
    lights_children : list = lights['children']

    for i in range(MAX_NUM_LIGHTS):
        token_prefix = f'scene.lights.{i}'
        if not config.has_option(UNNAMED_SECTION, f'{token_prefix}.name'):
            break

        name = config.get(UNNAMED_SECTION, f'{token_prefix}.name')
        type = config.getint(UNNAMED_SECTION, f'{token_prefix}.type')
        if type == 0:
            # directional
            rotation = light_dir_to_quaternion(parse_float_vector(config.get(UNNAMED_SECTION, f'{token_prefix}.direction')))
            color = parse_float_vector(config.get(UNNAMED_SECTION, f'{token_prefix}.color'))
            irridiance = config.getfloat(UNNAMED_SECTION, f'{token_prefix}.power')
            lights_children.append(
                {
                    "name": name,
                    "type": "DirectionalLight",
                    "rotation": rotation,
                    "angularSize": 1.0,
                    "color": color,
                    "irradiance": irridiance
                }
            )
        elif type == 1:
            # spot
            translation = parse_float_vector(config.get(UNNAMED_SECTION, f'{token_prefix}.position'))
            rotation = light_dir_to_quaternion(parse_float_vector(config.get(UNNAMED_SECTION, f'{token_prefix}.direction')))
            color = parse_float_vector(config.get(UNNAMED_SECTION, f'{token_prefix}.color'))
            intensity =config.getfloat(UNNAMED_SECTION, f'{token_prefix}.power')
            innerAngle = config.getfloat(UNNAMED_SECTION, f'{token_prefix}.penumbraAngle')
            outerAngle = config.getfloat(UNNAMED_SECTION, f'{token_prefix}.umbraAngle')
            radius = config.getfloat(UNNAMED_SECTION, f'{token_prefix}.radius')
            lights_children.append(
                {
                    "name": name,
                    "type": "SpotLight",
                    "translation": translation,
                    "rotation": rotation,
                    "color": color,
                    "intensity": intensity,
                    "innerAngle": innerAngle,
                    "outerAngle": outerAngle,
                    "radius": radius
                }
            )
        elif type == 2:
            # point
            translation = parse_float_vector(config.get(UNNAMED_SECTION, f'{token_prefix}.position'))
            color = parse_float_vector(config.get(UNNAMED_SECTION, f'{token_prefix}.color'))
            intensity = config.getfloat(UNNAMED_SECTION, f'{token_prefix}.power')
            radius = config.getfloat(UNNAMED_SECTION, f'{token_prefix}.radius')
            lights_children.append(
                {
                    "name": name,
                    "type": "PointLight",
                    "translation": translation,
                    "color": color,
                    "intensity": intensity,
                    "radius": radius
                }
            )

def cvt_scene_camera(config: ConfigParser, strm: dict):
    cameras = next((x for x in strm['graph'] if x['name'] == 'Cameras'), None)
    cameras_children : list = cameras['children']

    for i in range(MAX_NUM_CAMERAS):
        token_prefix = f'scene.cameras.{i}'
        if not config.has_option(UNNAMED_SECTION, f'{token_prefix}.name'):
            break

        name = config.get(UNNAMED_SECTION, f'{token_prefix}.name')
        translation = parse_float_vector(config.get(UNNAMED_SECTION, f'{token_prefix}.position'))
        pitch = config.getfloat(UNNAMED_SECTION, f'{token_prefix}.pitch')
        yaw = config.getfloat(UNNAMED_SECTION, f'{token_prefix}.yaw')
        rotation = Quaternion.from_euler_xyz([pitch, yaw, 0.0]).tolist()
        fov = config.getfloat(UNNAMED_SECTION, f'{token_prefix}.fov')
        aspect = config.getfloat(UNNAMED_SECTION, f'{token_prefix}.aspect')

        cameras_children.append(
            {
                "name": name,
                "type": "PerspectiveCamera",
                "translation": translation,
                "rotation": rotation,
                "verticalFov": fov,
                "zNear": 0.1
            }
        )

def cvt_scene(config: ConfigParser) -> dict:
    strm = default_scene_strm()
    cvt_model(config, strm)
    cvt_scene_lights(config, strm)
    cvt_scene_camera(config, strm)
    return strm

def default_config_strm():
    return {
        "scene": {
            "filePath": "",
            "skyRadiance": []
        },
        "interaction": {
        },
        "renderers": {
            "renderMode": "",
            "pathTracing": {
            },
            "rtao": {

            },
            "postProcessing": {
            },
            "DDGI": {

                "children": []
            }
        }
    }

def cvt_renderers_render_mode(config: ConfigParser, strm: dict):
    strm["renderers"]["renderMode"] = parse_render_mode(config.get(UNNAMED_SECTION, 'app.renderMode'))

def cvt_renderers_pathtracing(config: ConfigParser, strm: dict):
    strm["renderers"]["pathTracing"] = {
        "rayNormalBias": config.getfloat(UNNAMED_SECTION, 'pt.rayNormalBias'),
        "rayViewBias": config.getfloat(UNNAMED_SECTION, 'pt.rayViewBias'),
        "numBounces": config.getint(UNNAMED_SECTION, 'pt.numBounces'),
        "samplesPerPixel": config.getint(UNNAMED_SECTION, 'pt.samplesPerPixel'),
        "antiailiasing":  config.getboolean(UNNAMED_SECTION, 'pt.antialiasing')
    }

def cvt_renderers_rtao(config: ConfigParser, strm: dict):
    strm["renderers"]["rtao"] = {
        "enabled": config.getboolean(UNNAMED_SECTION, 'rtao.enable'),
        "rayLength": config.getfloat(UNNAMED_SECTION, 'rtao.rayLength'),
        "rayNormalBias": config.getfloat(UNNAMED_SECTION, 'rtao.rayNormalBias'),
        "rayViewBias": config.getfloat(UNNAMED_SECTION, 'rtao.rayViewBias'),
        "powerLog": config.getfloat(UNNAMED_SECTION, 'rtao.powerLog'),
        "filterDepthSigma": config.getfloat(UNNAMED_SECTION, 'rtao.filterDepthSigma'),
        "filterDistanceSigma": config.getfloat(UNNAMED_SECTION, 'rtao.filterDistanceSigma')
    }

def cvt_renderers_post_processing(config: ConfigParser, strm: dict):
    enabled = config.getboolean(UNNAMED_SECTION, 'pp.enable')
    strm["renderers"]["postProcessing"] = {
        "exposure": {
            "enabled": enabled and config.getboolean(UNNAMED_SECTION, 'pp.exposure.enable'),
            "fstops": config.getfloat(UNNAMED_SECTION, 'pp.exposure.fstops'),
        },
        "tonemapping.enabled": enabled and config.getboolean(UNNAMED_SECTION, 'pp.tonemap.enable'),
        "dithering.enabled": enabled and config.getboolean(UNNAMED_SECTION, 'pp.dither.enable'),
        "gammaCorrection.enabled": enabled and config.getboolean(UNNAMED_SECTION, 'pp.gamma.enable')
    }

def cvt_renderers_ddgi(config: ConfigParser, strm: dict):
    ddgi_volumes: list = strm["renderers"]["DDGI"]["children"]

    for i in range(MAX_NUM_DDGI_VOLUMES):
        token_prefix = f'ddgi.volume.{i}'

        if not config.has_option(UNNAMED_SECTION, f'{token_prefix}.name'):
            break

        ddgi_volumes.append(
            {
                "name": config.get(UNNAMED_SECTION, f'{token_prefix}.name'),
                "probeRelocation.enabled": config.getboolean(UNNAMED_SECTION, f'{token_prefix}.probeRelocation.enabled'),
                "probeRelocation.minFrontfaceDistance": rf(config.get(UNNAMED_SECTION, f'{token_prefix}.probeRelocation.minFrontfaceDistance')),
                "probeClassification.enabled": config.getboolean(UNNAMED_SECTION, f'{token_prefix}.probeClassification.enabled'),
                "probeVariability.enabled": config.getboolean(UNNAMED_SECTION, f'{token_prefix}.probeVariability.enabled'),
                "probeVariability.threshold": config.getfloat(UNNAMED_SECTION, f'{token_prefix}.probeVariability.threshold'),
                "infiniteScrolling.enabled": config.getboolean(UNNAMED_SECTION, f'{token_prefix}.infiniteScrolling.enabled'),
                "textures.rayData.format": parse_ddgi_format(config.get(UNNAMED_SECTION, f'{token_prefix}.textures.rayData.format')),
                "textures.irradiance.format": parse_ddgi_format(config.get(UNNAMED_SECTION, f'{token_prefix}.textures.irradiance.format')),
                "textures.distance.format": parse_ddgi_format(config.get(UNNAMED_SECTION, f'{token_prefix}.textures.distance.format')),
                "textures.data.format": parse_ddgi_format(config.get(UNNAMED_SECTION, f'{token_prefix}.textures.data.format')),
                "textures.variability.format": parse_ddgi_format(config.get(UNNAMED_SECTION, f'{token_prefix}.textures.variability.format')),
                "origin": parse_float_vector(config.get(UNNAMED_SECTION, f'{token_prefix}.origin')),
                "rotation": parse_float_vector(config.get(UNNAMED_SECTION, f'{token_prefix}.rotation', fallback='0 0 0')),
                "probeCounts": parse_int_vector(config.get(UNNAMED_SECTION, f'{token_prefix}.probeCounts')),
                "probeSpacing": parse_float_vector(config.get(UNNAMED_SECTION, f'{token_prefix}.probeSpacing')),
                "probeNumRays": config.getint(UNNAMED_SECTION, f'{token_prefix}.probeNumRays'),
                "probeNumIrradianceTexels": config.getint(UNNAMED_SECTION, f'{token_prefix}.probeNumIrradianceTexels'),
                "probeNumDistanceTexels": config.getint(UNNAMED_SECTION, f'{token_prefix}.probeNumDistanceTexels'),
                "probeHysteresis": config.getfloat(UNNAMED_SECTION, f'{token_prefix}.probeHysteresis'),
                "probeNormalBias": config.getfloat(UNNAMED_SECTION, f'{token_prefix}.probeNormalBias'),
                "probeViewBias": config.getfloat(UNNAMED_SECTION, f'{token_prefix}.probeViewBias'),
                "probeMaxRayDistance": config.getfloat(UNNAMED_SECTION, f'{token_prefix}.probeMaxRayDistance'),
                "probeIrradianceThreshold": config.getfloat(UNNAMED_SECTION, f'{token_prefix}.probeIrradianceThreshold'),
                "probeBrightnessThreshold": config.getfloat(UNNAMED_SECTION, f'{token_prefix}.probeBrightnessThreshold'),
                "vis.probeVisType": parse_ddgi_vis_type(config.getint(UNNAMED_SECTION, f'{token_prefix}.vis.probeVisType')),
                "vis.probeRadius": config.getfloat(UNNAMED_SECTION, f'{token_prefix}.vis.probeRadius'),
                "vis.probeDistanceDivisor": config.getfloat(UNNAMED_SECTION, f'{token_prefix}.vis.probeDistanceDivisor'),
                "vis.showProbes": config.getboolean(UNNAMED_SECTION, f'{token_prefix}.vis.showProbes'),
                "vis.texture.irradianceScale": config.getfloat(UNNAMED_SECTION, f'{token_prefix}.vis.texture.irradianceScale'),
                "vis.texture.distanceScale": config.getfloat(UNNAMED_SECTION, f'{token_prefix}.vis.texture.distanceScale'),
                "vis.texture.probeDataScale": config.getfloat(UNNAMED_SECTION, f'{token_prefix}.vis.texture.probeDataScale'),
                "vis.texture.rayDataScale": config.getfloat(UNNAMED_SECTION, f'{token_prefix}.vis.texture.rayDataScale'),
                "vis.texture.probeVariabilityScale": config.getfloat(UNNAMED_SECTION, f'{token_prefix}.vis.texture.probeVariabilityScale')
            }
        )

def cvt_renderers(config: ConfigParser, strm: dict):
    cvt_renderers_render_mode(config, strm)
    cvt_renderers_pathtracing(config, strm)
    cvt_renderers_rtao(config, strm)
    cvt_renderers_post_processing(config, strm)
    cvt_renderers_ddgi(config, strm)

def cvt_interaction(config: ConfigParser, strm: dict):
    strm["interaction"] = {
        "mouse.movementSpeed": rf(config.get(UNNAMED_SECTION, 'input.movementSpeed')),
        "mouse.rotationSpeed": rf(config.get(UNNAMED_SECTION, 'input.rotationSpeed')),
        "mouse.invertPan": config.getboolean(UNNAMED_SECTION, 'input.invertPan')
    }

def cvt_scene_config(config: ConfigParser, scn_path: str, strm: dict):
    strm["scene"] = {
        "filePath": scn_fpath,
        "skyRadiance": (np.array(parse_float_vector(config.get(UNNAMED_SECTION, 'scene.skyColor')))
                        * config.getfloat(UNNAMED_SECTION, 'scene.skyIntensity')).tolist()
    }

def cvt_config(config: ConfigParser, scn_fpath: str):
    strm = default_config_strm()
    cvt_scene_config(config, scn_fpath, strm)
    cvt_renderers(config, strm)
    cvt_interaction(config, strm)
    return strm

def cvt_direction_to_quaternion(v, init_v):
    return Quaternion.from_two_vectors(init_v / np.sqrt(np.dot(init_v, init_v)), v / np.sqrt(np.dot(v, v)))

if __name__ == '__main__':

    print(f'{cvt_direction_to_quaternion([-20.0, -30.0, 2.0], [0.0, 0.0, -1.0])}')
    print(f'{cvt_direction_to_quaternion([-1.0, 0.0, 0.0], [0.0, 0.0, 1.0])}')

    if False:
        config_dir = sys.argv[1]
        out_dir = sys.argv[2]

        config_files = [path.join(config_dir, f) for f in os.listdir(config_dir) if path.isfile(path.join(config_dir, f))]

        for config_fpath in config_files:
            config = ConfigParser(allow_unnamed_section=True)
            config.read(config_fpath)

            out_strm = cvt_scene(config)

            config_name = path.splitext(path.basename(config_fpath))[0]

            out_fpath = path.join(out_dir, config_name + ".scene.json")
            with open(out_fpath, mode='w') as out_fp:
                out_fp.write(json.dumps(out_strm, indent=4))

            scn_fpath = path.basename(out_fpath)
            out_strm = cvt_config(config, scn_fpath)

            out_fpath = path.join(out_dir, config_name + ".cfg.json")
            with open(out_fpath, mode='w') as out_fp:
                out_fp.write(json.dumps(out_strm, indent=4))