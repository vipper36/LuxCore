/***************************************************************************
 * Copyright 1998-2013 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxRender.                                       *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 *     http://www.apache.org/licenses/LICENSE-2.0                          *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#include <memory>
#include <boost/thread.hpp>
#include <boost/lexical_cast.hpp>

#include "slg/renderconfig.h"
#include "slg/renderengine.h"
#include "slg/engines/rtpathocl/rtpathocl.h"
#include "slg/engines/rtbiaspathocl/rtbiaspathocl.h"
#include "slg/engines/lightcpu/lightcpu.h"
#include "slg/engines/pathcpu/pathcpu.h"
#include "slg/engines/bidircpu/bidircpu.h"
#include "slg/engines/bidirhybrid/bidirhybrid.h"
#include "slg/engines/cbidirhybrid/cbidirhybrid.h"
#include "slg/engines/bidirvmcpu/bidirvmcpu.h"
#include "slg/engines/filesaver/filesaver.h"
#include "slg/engines/pathhybrid/pathhybrid.h"
#include "slg/engines/biaspathcpu/biaspathcpu.h"
#include "slg/engines/biaspathocl/biaspathocl.h"

using namespace std;
using namespace luxrays;
using namespace slg;

static boost::mutex defaultPropertiesMutex;
static auto_ptr<Properties> defaultProperties;

void RenderConfig::InitDefaultProperties() {
	// Check if I have to initialize the default Properties
	if (!defaultProperties.get()) {
		boost::unique_lock<boost::mutex> lock(defaultPropertiesMutex);
		if (!defaultProperties.get()) {
			defaultProperties.reset(new Properties());

			defaultProperties->Set(Property("accelerator.instances.enable")(true));
			defaultProperties->Set(Property("accelerator.type")("AUTO"));
			// Film Filter
			defaultProperties->Set(Property("film.filter.type")("GAUSSIAN"));
			defaultProperties->Set(Property("film.filter.width")(1.5f));
			defaultProperties->Set(Property("film.filter.gaussian.alpha")(2.f));
			defaultProperties->Set(Property("film.filter.mitchell.b")(1.f / 3.f));
			defaultProperties->Set(Property("film.filter.mitchell.c")(1.f / 3.f));
			defaultProperties->Set(Property("film.filter.mitchellss.b")(1.f / 3.f));
			defaultProperties->Set(Property("film.filter.mitchellss.c")(1.f / 3.f));
			// Film ToneMap
			defaultProperties->Set(Property("film.tonemap.type")("LINEAR"));
			LinearToneMapParams toneMapLinear;
			defaultProperties->Set(Property("film.tonemap.linear.scale")(toneMapLinear.scale));
			Reinhard02ToneMapParams toneMapReinhard02;
			defaultProperties->Set(Property("film.tonemap.reinhard02.prescale")(toneMapReinhard02.preScale));
			defaultProperties->Set(Property("film.tonemap.reinhard02.postscale")(toneMapReinhard02.postScale));
			defaultProperties->Set(Property("film.tonemap.reinhard02.burn")(toneMapReinhard02.burn));

			defaultProperties->Set(Property("film.gamma")(2.2f));
			defaultProperties->Set(Property("film.height")(480u));
			defaultProperties->Set(Property("film.width")(640u));

			// Sampler
			defaultProperties->Set(Property("sampler.type")("RANDOM"));
			defaultProperties->Set(Property("sampler.metropolis.largesteprate")(.4f));
			defaultProperties->Set(Property("sampler.metropolis.maxconsecutivereject")(512));
			defaultProperties->Set(Property("sampler.metropolis.imagemutationrate")(.1f));
			
			defaultProperties->Set(Property("images.scale")(1.f));
			defaultProperties->Set(Property("renderengine.type")("PATHOCL"));
			defaultProperties->Set(Property("scene.file")("scenes/luxball/luxball.scn"));
			defaultProperties->Set(Property("screen.refresh.interval")(100u));

			// Specific RenderEngine settings are defined in each RenderEngine::Start() method
		}
	}
}

const Properties &RenderConfig::GetDefaultProperties() {
	InitDefaultProperties();

	return *defaultProperties;
	
}

RenderConfig::RenderConfig(const luxrays::Properties &props, Scene *scn) : scene(scn) {
	InitDefaultProperties();

	SLG_LOG("Configuration: ");
	const vector<string> &keys = props.GetAllNames();
	for (vector<string>::const_iterator i = keys.begin(); i != keys.end(); ++i)
		SLG_LOG("  " << props.Get(*i));

	// Set the Scene
	if (scn) {
		scene = scn;
		allocatedScene = false;
	} else {
		// Create the Scene
		const string sceneFileName = GetProperty("scene.file").Get<string>();
		const float imageScale = Max(.01f, GetProperty("images.scale").Get<float>());

		scene = new Scene(sceneFileName, imageScale);
		allocatedScene = true;
	}

	// Parse the configuration
	Parse(props);
}

RenderConfig::~RenderConfig() {
	// Check if the scene was allocated by me
	if (allocatedScene)
		delete scene;
}

const luxrays::Property RenderConfig::GetProperty(const std::string &name) const {
	if (cfg.IsDefined(name))
		return cfg.Get(name);
	else {
		// Use the default value
		return defaultProperties->Get(name);
	}
}

void RenderConfig::Parse(const luxrays::Properties &props) {
	cfg.Set(props);

	scene->enableInstanceSupport = GetProperty("accelerator.instances.enable").Get<bool>();
	const string accelType = GetProperty("accelerator.type").Get<string>();
	// "-1" is for compatibility with the past. However all other old values are
	// not emulated (i.e. the "AUTO" behavior is preferred in that case)
	if ((accelType == "AUTO") || (accelType == "-1"))
		scene->accelType = ACCEL_AUTO;
	else if (accelType == "BVH")
		scene->accelType = ACCEL_BVH;
	else if (accelType == "MBVH")
		scene->accelType = ACCEL_MBVH;
	else if (accelType == "QBVH")
		scene->accelType = ACCEL_QBVH;
	else if (accelType == "MQBVH")
		scene->accelType = ACCEL_MQBVH;
	else {
		SLG_LOG("Unknown accelerator type (using AUTO instead): " << accelType);
	}

	// Update the Camera
	u_int filmFullWidth, filmFullHeight, filmSubRegion[4];
	u_int *subRegion = GetFilmSize(&filmFullWidth, &filmFullHeight, filmSubRegion) ?
		filmSubRegion : NULL;
	scene->camera->Update(filmFullWidth, filmFullHeight, subRegion);
}

void RenderConfig::Delete(const string prefix) {
	cfg.DeleteAll(cfg.GetAllNames(prefix));
}

bool RenderConfig::GetFilmSize(u_int *filmFullWidth, u_int *filmFullHeight,
		u_int *filmSubRegion) const {
	u_int width = 640;
	if (cfg.IsDefined("image.width")) {
		SLG_LOG("WARNING: deprecated property image.width");
		width = cfg.Get(Property("image.width")(width)).Get<u_int>();
	}
	width = GetProperty("film.width").Get<u_int>();

	u_int height = 480;
	if (cfg.IsDefined("image.height")) {
		SLG_LOG("WARNING: deprecated property image.height");
		height = cfg.Get(Property("image.height")(height)).Get<u_int>();
	}
	height = GetProperty("film.height").Get<u_int>();

	// Check if I'm rendering a film subregion
	u_int subRegion[4];
	bool subRegionUsed;
	if (cfg.IsDefined("film.subregion")) {
		const Property &prop = cfg.Get(Property("film.subregion")(0, width - 1u, 0, height - 1u));

		subRegion[0] = Max(0u, Min(width - 1, prop.Get<u_int>(0)));
		subRegion[1] = Max(0u, Min(width - 1, Max(subRegion[0] + 1, prop.Get<u_int>(1))));
		subRegion[2] = Max(0u, Min(height - 1, prop.Get<u_int>(2)));
		subRegion[3] = Max(0u, Min(height - 1, Max(subRegion[2] + 1, prop.Get<u_int>(3))));
		subRegionUsed = true;
	} else {
		subRegion[0] = 0;
		subRegion[1] = width - 1;
		subRegion[2] = 0;
		subRegion[3] = height - 1;
		subRegionUsed = false;
	}

	if (filmFullWidth)
		*filmFullWidth = width;
	if (filmFullHeight)
		*filmFullHeight = height;

	if (filmSubRegion) {
		filmSubRegion[0] = subRegion[0];
		filmSubRegion[1] = subRegion[1];
		filmSubRegion[2] = subRegion[2];
		filmSubRegion[3] = subRegion[3];
	}

	return subRegionUsed;
}

Film *RenderConfig::AllocFilm(FilmOutputs &filmOutputs) const {
	//--------------------------------------------------------------------------
	// Create the filter
	//--------------------------------------------------------------------------

	const FilterType filterType = Filter::String2FilterType(GetProperty("film.filter.type").Get<string>());
	const float filterWidth = GetProperty("film.filter.width").Get<float>();

	auto_ptr<Filter> filter;
	switch (filterType) {
		case FILTER_NONE:
			break;
		case FILTER_BOX:
			filter.reset(new BoxFilter(filterWidth, filterWidth));
			break;
		case FILTER_GAUSSIAN: {
			const float alpha = GetProperty("film.filter.gaussian.alpha").Get<float>();
			filter.reset(new GaussianFilter(filterWidth, filterWidth, alpha));
			break;
		}
		case FILTER_MITCHELL: {
			const float b = GetProperty("film.filter.mitchell.b").Get<float>();
			const float c = GetProperty("film.filter.mitchell.c").Get<float>();
			filter.reset(new MitchellFilter(filterWidth, filterWidth, b, c));
			break;
		}
		case FILTER_MITCHELL_SS: {
			const float b = GetProperty("film.filter.mitchellss.b").Get<float>();
			const float c = GetProperty("film.filter.mitchellss.c").Get<float>();
			filter.reset(new MitchellFilterSS(filterWidth, filterWidth, b, c));
			break;
		}
		default:
			throw runtime_error("Unknown filter type: " + boost::lexical_cast<string>(filterType));
	}

	//--------------------------------------------------------------------------
	// Create the Film
	//--------------------------------------------------------------------------

	u_int filmFullWidth, filmFullHeight, filmSubRegion[4];
	GetFilmSize(&filmFullWidth, &filmFullHeight, filmSubRegion);

	SDL_LOG("Film resolution: " << filmFullWidth << "x" << filmFullHeight);
	auto_ptr<Film> film(new Film(filmFullWidth, filmFullHeight));
	film->SetFilter(filter.release());

	const ToneMapType toneMapType = String2ToneMapType(GetProperty("film.tonemap.type").Get<string>());
	switch (toneMapType) {
		case TONEMAP_LINEAR: {
			LinearToneMapParams params;
			params.scale = GetProperty("film.tonemap.linear.scale").Get<float>();
			film->SetToneMapParams(params);
			break;
		}
		case TONEMAP_REINHARD02: {
			Reinhard02ToneMapParams params;
			params.preScale = GetProperty("film.tonemap.reinhard02.prescale").Get<float>();
			params.postScale = GetProperty("film.tonemap.reinhard02.postscale").Get<float>();
			params.burn = GetProperty("film.tonemap.reinhard02.burn").Get<float>();
			film->SetToneMapParams(params);
			break;
		}
		default:
			throw runtime_error("Unknown tone mapping type: " + boost::lexical_cast<string>(toneMapType));
	}

	const float gamma = GetProperty("film.gamma").Get<float>();
	if (gamma != 2.2f)
		film->SetGamma(gamma);

	// For compatibility with the past
	if (cfg.IsDefined("film.alphachannel.enable")) {
		SLG_LOG("WARNING: deprecated property film.alphachannel.enable");

		if (cfg.Get(Property("film.alphachannel.enable")(0)).Get<bool>())
			film->AddChannel(Film::ALPHA);
		else
			film->RemoveChannel(Film::ALPHA);
	}

	//--------------------------------------------------------------------------
	// Initialize the FilmOutputs
	//--------------------------------------------------------------------------

	set<string> outputNames;
	vector<string> outputKeys = cfg.GetAllNames("film.outputs.");
	for (vector<string>::const_iterator outputKey = outputKeys.begin(); outputKey != outputKeys.end(); ++outputKey) {
		const string &key = *outputKey;
		const size_t dot1 = key.find(".", string("film.outputs.").length());
		if (dot1 == string::npos)
			continue;

		// Extract the output type name
		const string outputName = Property::ExtractField(key, 2);
		if (outputName == "")
			throw runtime_error("Syntax error in film output definition: " + outputName);

		if (outputNames.count(outputName) > 0)
			continue;

		outputNames.insert(outputName);
		const string type = cfg.Get(Property("film.outputs." + outputName + ".type")("RGB_TONEMAPPED")).Get<string>();
		const string fileName = cfg.Get(Property("film.outputs." + outputName + ".filename")("image.png")).Get<string>();

		SDL_LOG("Film output definition: " << type << " [" << fileName << "]");

		// Check if it is a supported file format
		FREE_IMAGE_FORMAT fif = FREEIMAGE_GETFIFFROMFILENAME(FREEIMAGE_CONVFILENAME(fileName).c_str());
		if (fif == FIF_UNKNOWN)
			throw runtime_error("Unknown image format in film output: " + outputName);

		// HDR image or not
		const bool hdrImage = ((fif == FIF_HDR) || (fif == FIF_EXR));

		if (type == "RGB") {
			if (hdrImage)
				filmOutputs.Add(FilmOutputs::RGB, fileName);
			else
				throw runtime_error("Not tonemapped image can be saved only in HDR formats: " + outputName);
		} else if (type == "RGBA") {
			if (hdrImage) {
				film->AddChannel(Film::ALPHA);
				filmOutputs.Add(FilmOutputs::RGBA, fileName);
			} else
				throw runtime_error("Not tonemapped image can be saved only in HDR formats: " + outputName);
		} else if (type == "RGB_TONEMAPPED")
			filmOutputs.Add(FilmOutputs::RGB_TONEMAPPED, fileName);
		else if (type == "RGBA_TONEMAPPED") {
			film->AddChannel(Film::ALPHA);
			filmOutputs.Add(FilmOutputs::RGBA_TONEMAPPED, fileName);
		} else if (type == "ALPHA") {
			film->AddChannel(Film::ALPHA);
			filmOutputs.Add(FilmOutputs::ALPHA, fileName);
		} else if (type == "DEPTH") {
			if (hdrImage) {
				film->AddChannel(Film::DEPTH);
				filmOutputs.Add(FilmOutputs::DEPTH, fileName);
			} else
				throw runtime_error("Depth image can be saved only in HDR formats: " + outputName);
		} else if (type == "POSITION") {
			if (hdrImage) {
				film->AddChannel(Film::DEPTH);
				film->AddChannel(Film::POSITION);
				filmOutputs.Add(FilmOutputs::POSITION, fileName);
			} else
				throw runtime_error("Position image can be saved only in HDR formats: " + outputName);
		} else if (type == "GEOMETRY_NORMAL") {
			if (hdrImage) {
				film->AddChannel(Film::DEPTH);
				film->AddChannel(Film::GEOMETRY_NORMAL);
				filmOutputs.Add(FilmOutputs::GEOMETRY_NORMAL, fileName);
			} else
				throw runtime_error("Geometry normal image can be saved only in HDR formats: " + outputName);
		} else if (type == "SHADING_NORMAL") {
			if (hdrImage) {
				film->AddChannel(Film::DEPTH);
				film->AddChannel(Film::SHADING_NORMAL);
				filmOutputs.Add(FilmOutputs::SHADING_NORMAL, fileName);
			} else
				throw runtime_error("Shading normal image can be saved only in HDR formats: " + outputName);
		} else if (type == "MATERIAL_ID") {
			if (!hdrImage) {
				film->AddChannel(Film::DEPTH);
				film->AddChannel(Film::MATERIAL_ID);
				filmOutputs.Add(FilmOutputs::MATERIAL_ID, fileName);
			} else
				throw runtime_error("Material ID image can be saved only in no HDR formats: " + outputName);
		} else if (type == "DIRECT_DIFFUSE") {
			if (hdrImage) {
				film->AddChannel(Film::DIRECT_DIFFUSE);
				filmOutputs.Add(FilmOutputs::DIRECT_DIFFUSE, fileName);
			} else
				throw runtime_error("Direct diffuse image can be saved only in HDR formats: " + outputName);
		} else if (type == "DIRECT_GLOSSY") {
			if (hdrImage) {
				film->AddChannel(Film::DIRECT_GLOSSY);
				filmOutputs.Add(FilmOutputs::DIRECT_GLOSSY, fileName);
			} else
				throw runtime_error("Direct glossy image can be saved only in HDR formats: " + outputName);
		} else if (type == "EMISSION") {
			if (hdrImage) {
				film->AddChannel(Film::EMISSION);
				filmOutputs.Add(FilmOutputs::EMISSION, fileName);
			} else
				throw runtime_error("Emission image can be saved only in HDR formats: " + outputName);
		} else if (type == "INDIRECT_DIFFUSE") {
			if (hdrImage) {
				film->AddChannel(Film::INDIRECT_DIFFUSE);
				filmOutputs.Add(FilmOutputs::INDIRECT_DIFFUSE, fileName);
			} else
				throw runtime_error("Indirect diffuse image can be saved only in HDR formats: " + outputName);
		} else if (type == "INDIRECT_GLOSSY") {
			if (hdrImage) {
				film->AddChannel(Film::INDIRECT_GLOSSY);
				filmOutputs.Add(FilmOutputs::INDIRECT_GLOSSY, fileName);
			} else
				throw runtime_error("Indirect glossy image can be saved only in HDR formats: " + outputName);
		} else if (type == "INDIRECT_SPECULAR") {
			if (hdrImage) {
				film->AddChannel(Film::INDIRECT_SPECULAR);
				filmOutputs.Add(FilmOutputs::INDIRECT_SPECULAR, fileName);
			} else
				throw runtime_error("Indirect specular image can be saved only in HDR formats: " + outputName);
		} else if (type == "MATERIAL_ID_MASK") {
			const u_int materialID = cfg.Get(Property("film.outputs." + outputName + ".id")(255)).Get<u_int>();
			Properties prop;
			prop.Set(Property("id")(materialID));

			film->AddChannel(Film::MATERIAL_ID);
			film->AddChannel(Film::MATERIAL_ID_MASK, &prop);
			filmOutputs.Add(FilmOutputs::MATERIAL_ID_MASK, fileName, &prop);
		} else if (type == "DIRECT_SHADOW_MASK") {
			film->AddChannel(Film::DIRECT_SHADOW_MASK);
			filmOutputs.Add(FilmOutputs::DIRECT_SHADOW_MASK, fileName);
		} else if (type == "INDIRECT_SHADOW_MASK") {
			film->AddChannel(Film::INDIRECT_SHADOW_MASK);
			filmOutputs.Add(FilmOutputs::INDIRECT_SHADOW_MASK, fileName);
		} else if (type == "RADIANCE_GROUP") {
			const u_int lightID = cfg.Get(Property("film.outputs." + outputName + ".id")(0)).Get<u_int>();
			Properties prop;
			prop.Set(Property("id")(lightID));

			filmOutputs.Add(FilmOutputs::RADIANCE_GROUP, fileName, &prop);
		} else if (type == "UV") {
			film->AddChannel(Film::DEPTH);
			film->AddChannel(Film::UV);
			filmOutputs.Add(FilmOutputs::UV, fileName);
		} else if (type == "RAYCOUNT") {
			film->AddChannel(Film::RAYCOUNT);
			filmOutputs.Add(FilmOutputs::RAYCOUNT, fileName);
		} else
			throw runtime_error("Unknown type in film output: " + type);
	}

	// For compatibility with the past
	if (cfg.IsDefined("image.filename")) {
		SLG_LOG("WARNING: deprecated property image.filename");
		filmOutputs.Add(film->HasChannel(Film::ALPHA) ? FilmOutputs::RGBA_TONEMAPPED : FilmOutputs::RGB_TONEMAPPED,
				cfg.Get(Property("image.filename")("image.png")).Get<string>());
	}

	// Default setting
	if (filmOutputs.GetCount() == 0)
		filmOutputs.Add(FilmOutputs::RGB_TONEMAPPED, "image.png");

	return film.release();
}

Sampler *RenderConfig::AllocSampler(RandomGenerator *rndGen, Film *film,
		double *metropolisSharedTotalLuminance, double *metropolisSharedSampleCount) const {
	const SamplerType samplerType = Sampler::String2SamplerType(GetProperty("sampler.type").Get<string>());
	switch (samplerType) {
		case RANDOM:
			return new RandomSampler(rndGen, film);
		case METROPOLIS: {
			const float rate = GetProperty("sampler.metropolis.largesteprate").Get<float>();
			const float reject = GetProperty("sampler.metropolis.maxconsecutivereject").Get<float>();
			const float mutationrate = GetProperty("sampler.metropolis.imagemutationrate").Get<float>();

			return new MetropolisSampler(rndGen, film, reject, rate, mutationrate,
					metropolisSharedTotalLuminance, metropolisSharedSampleCount);
		}
		case SOBOL:
			return new SobolSampler(rndGen, film);
		default:
			throw runtime_error("Unknown sampler.type: " + boost::lexical_cast<string>(samplerType));
	}
}

RenderEngine *RenderConfig::AllocRenderEngine(Film *film, boost::mutex *filmMutex) const {
	const RenderEngineType renderEngineType = RenderEngine::String2RenderEngineType(
		GetProperty("renderengine.type").Get<string>());

	switch (renderEngineType) {
		case LIGHTCPU:
			return new LightCPURenderEngine(this, film, filmMutex);
		case PATHOCL:
#ifndef LUXRAYS_DISABLE_OPENCL
			return new PathOCLRenderEngine(this, film, filmMutex);
#else
			SLG_LOG("OpenCL unavailable, falling back to CPU rendering");
#endif
		case PATHCPU:
			return new PathCPURenderEngine(this, film, filmMutex);
		case BIDIRCPU:
			return new BiDirCPURenderEngine(this, film, filmMutex);
		case BIDIRHYBRID:
			return new BiDirHybridRenderEngine(this, film, filmMutex);
		case CBIDIRHYBRID:
			return new CBiDirHybridRenderEngine(this, film, filmMutex);
		case BIDIRVMCPU:
			return new BiDirVMCPURenderEngine(this, film, filmMutex);
		case FILESAVER:
			return new FileSaverRenderEngine(this, film, filmMutex);
		case RTPATHOCL:
#ifndef LUXRAYS_DISABLE_OPENCL
			return new RTPathOCLRenderEngine(this, film, filmMutex);
#else
			SLG_LOG("OpenCL unavailable, falling back to CPU rendering");
			return new PathCPURenderEngine(this, film, filmMutex);
#endif
		case PATHHYBRID:
			return new PathHybridRenderEngine(this, film, filmMutex);
		case BIASPATHCPU:
			return new BiasPathCPURenderEngine(this, film, filmMutex);
		case BIASPATHOCL:
#ifndef LUXRAYS_DISABLE_OPENCL
			return new BiasPathOCLRenderEngine(this, film, filmMutex);
#else
			SLG_LOG("OpenCL unavailable, falling back to CPU rendering");
			return new BiasPathCPURenderEngine(this, film, filmMutex);
#endif
		case RTBIASPATHOCL:
#ifndef LUXRAYS_DISABLE_OPENCL
			return new RTBiasPathOCLRenderEngine(this, film, filmMutex);
#else
			SLG_LOG("OpenCL unavailable, falling back to CPU rendering");
			return new BiasPathCPURenderEngine(this, film, filmMutex);
#endif
		default:
			throw runtime_error("Unknown render engine type: " + boost::lexical_cast<string>(renderEngineType));
	}
}
