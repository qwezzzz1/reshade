/**
 * Copyright (C) 2014 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/reshade#license
 */

#include "d3d9_runtime.hpp"
#include "d3d9_effect_compiler.hpp"
#include <assert.h>
#include <iomanip>
#include <fstream>
#include <algorithm>
#include <d3dcompiler.h>

namespace reshade::d3d9
{
	using namespace reshadefx;

	static D3DBLEND literal_to_blend_func(unsigned int value)
	{
		switch (value)
		{
		case 0:
			return D3DBLEND_ZERO;
		default:
		case 1:
			return D3DBLEND_ONE;
		case 2:
			return D3DBLEND_SRCCOLOR;
		case 4:
			return D3DBLEND_INVSRCCOLOR;
		case 3:
			return D3DBLEND_SRCALPHA;
		case 5:
			return D3DBLEND_INVSRCALPHA;
		case 6:
			return D3DBLEND_DESTALPHA;
		case 7:
			return D3DBLEND_INVDESTALPHA;
		case 8:
			return D3DBLEND_DESTCOLOR;
		case 9:
			return D3DBLEND_INVDESTCOLOR;
		}
	}
	static D3DSTENCILOP literal_to_stencil_op(unsigned int value)
	{
		switch (value)
		{
		default:
		case 1:
			return D3DSTENCILOP_KEEP;
		case 0:
			return D3DSTENCILOP_ZERO;
		case 3:
			return D3DSTENCILOP_REPLACE;
		case 4:
			return D3DSTENCILOP_INCRSAT;
		case 5:
			return D3DSTENCILOP_DECRSAT;
		case 6:
			return D3DSTENCILOP_INVERT;
		case 7:
			return D3DSTENCILOP_INCR;
		case 8:
			return D3DSTENCILOP_DECR;
		}
	}
	static D3DFORMAT literal_to_format(texture_format value)
	{
		switch (value)
		{
		case texture_format::r8:
			return D3DFMT_A8R8G8B8;
		case texture_format::r16f:
			return D3DFMT_R16F;
		case texture_format::r32f:
			return D3DFMT_R32F;
		case texture_format::rg8:
			return D3DFMT_A8R8G8B8;
		case texture_format::rg16:
			return D3DFMT_G16R16;
		case texture_format::rg16f:
			return D3DFMT_G16R16F;
		case texture_format::rg32f:
			return D3DFMT_G32R32F;
		case texture_format::rgba8:
			return D3DFMT_A8R8G8B8;
		case texture_format::rgba16:
			return D3DFMT_A16B16G16R16;
		case texture_format::rgba16f:
			return D3DFMT_A16B16G16R16F;
		case texture_format::rgba32f:
			return D3DFMT_A32B32G32R32F;
		case texture_format::dxt1:
			return D3DFMT_DXT1;
		case texture_format::dxt3:
			return D3DFMT_DXT3;
		case texture_format::dxt5:
			return D3DFMT_DXT5;
		case texture_format::latc1:
			return static_cast<D3DFORMAT>(MAKEFOURCC('A', 'T', 'I', '1'));
		case texture_format::latc2:
			return static_cast<D3DFORMAT>(MAKEFOURCC('A', 'T', 'I', '2'));
		}

		return D3DFMT_UNKNOWN;
	}

	static void copy_annotations(const std::unordered_map<std::string, std::pair<type, constant>> &source, std::unordered_map<std::string, variant> &target)
	{
		for (const auto &annotation : source)
			switch (annotation.second.first.base)
			{
			case type::t_int:
				target.insert({ annotation.first, variant(annotation.second.second.as_int[0]) });
				break;
			case type::t_bool:
			case type::t_uint:
				target.insert({ annotation.first, variant(annotation.second.second.as_uint[0]) });
				break;
			case type::t_float:
				target.insert({ annotation.first, variant(annotation.second.second.as_float[0]) });
				break;
			case type::t_string:
				target.insert({ annotation.first, variant(annotation.second.second.string_data) });
				break;
			}
	}

	d3d9_effect_compiler::d3d9_effect_compiler(d3d9_runtime *runtime, const module &module, std::string &errors) :
		_runtime(runtime),
		_module(&module),
		_errors(errors)
	{
	}

	bool d3d9_effect_compiler::run()
	{
		_d3dcompiler_module = LoadLibraryW(L"d3dcompiler_47.dll");

		if (_d3dcompiler_module == nullptr)
		{
			_d3dcompiler_module = LoadLibraryW(L"d3dcompiler_43.dll");
		}
		if (_d3dcompiler_module == nullptr)
		{
			_errors += "Unable to load D3DCompiler library. Make sure you have the DirectX end-user runtime (June 2010) installed or a newer version of the library in the application directory.\n";
			return false;
		}

		// Parse uniform variables
		_uniform_storage_offset = _runtime->get_uniform_value_storage().size();

		for (const auto &texture : _module->textures)
		{
			visit_texture(texture);
		}
		for (const auto &sampler : _module->samplers)
		{
			visit_sampler(sampler);
		}
		for (const auto &uniform : _module->uniforms)
		{
			visit_uniform(uniform);
		}

		// Compile all entry points
		for (const auto &entry : _module->entry_points)
		{
			compile_entry_point(entry.first, entry.second);
		}

		// Parse technique information
		for (const auto &technique : _module->techniques)
		{
			visit_technique(technique);
		}

		FreeLibrary(_d3dcompiler_module);

		return _success;
	}

	void d3d9_effect_compiler::error(const std::string &message)
	{
		_success = false;

		_errors += "error: " + message + '\n';
	}
	void d3d9_effect_compiler::warning(const std::string &message)
	{
		_errors += "warning: " + message + '\n';
	}

	void d3d9_effect_compiler::visit_texture(const texture_info &texture_info)
	{
		const auto existing_texture = _runtime->find_texture(texture_info.unique_name);

		if (existing_texture != nullptr)
		{
			if (texture_info.semantic.empty() && (
				existing_texture->width != texture_info.width ||
				existing_texture->height != texture_info.height ||
				existing_texture->levels != texture_info.levels ||
				existing_texture->format != static_cast<texture_format>(texture_info.format)))
				error(existing_texture->effect_filename + " already created a texture with the same name but different dimensions; textures are shared across all effects, so either rename the variable or adjust the dimensions so they match");
			return;
		}

		texture obj;
		obj.unique_name = texture_info.unique_name;
		copy_annotations(texture_info.annotations, obj.annotations);
		obj.width = texture_info.width;
		obj.height = texture_info.height;
		obj.levels = texture_info.levels;
		obj.format = static_cast<texture_format>(texture_info.format);

		const D3DFORMAT format = literal_to_format(obj.format);

		obj.impl = std::make_unique<d3d9_tex_data>();
		const auto obj_data = obj.impl->as<d3d9_tex_data>();

		if (texture_info.semantic == "COLOR")
		{
			_runtime->update_texture_reference(obj, texture_reference::back_buffer);
		}
		else if (texture_info.semantic == "DEPTH")
		{
			_runtime->update_texture_reference(obj, texture_reference::depth_buffer);
		}
		else if (!texture_info.semantic.empty())
		{
			error("invalid semantic");
			return;
		}
		else
		{
			UINT levels = obj.levels;
			DWORD usage = 0;
			D3DDEVICE_CREATION_PARAMETERS cp;
			_runtime->_device->GetCreationParameters(&cp);

			if (levels > 1)
			{
				if (_runtime->_d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, D3DFMT_X8R8G8B8, D3DUSAGE_AUTOGENMIPMAP, D3DRTYPE_TEXTURE, format) == D3D_OK)
				{
					usage |= D3DUSAGE_AUTOGENMIPMAP;
					levels = 0;
				}
				else
				{
					warning("autogenerated miplevels are not supported for this format");
				}
			}

			HRESULT hr = _runtime->_d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, format);

			if (SUCCEEDED(hr))
			{
				usage |= D3DUSAGE_RENDERTARGET;
			}

			hr = _runtime->_device->CreateTexture(obj.width, obj.height, levels, usage, format, D3DPOOL_DEFAULT, &obj_data->texture, nullptr);

			if (FAILED(hr))
			{
				error("internal texture creation failed with error code " + std::to_string(static_cast<unsigned long>(hr)) + "!");
				return;
			}

			hr = obj_data->texture->GetSurfaceLevel(0, &obj_data->surface);

			assert(SUCCEEDED(hr));
		}

		_runtime->add_texture(std::move(obj));
	}

	void d3d9_effect_compiler::visit_sampler(const sampler_info &sampler_info)
	{
		const auto existing_texture = _runtime->find_texture(sampler_info.texture_name);

		if (!existing_texture)
			return;

		d3d9_sampler sampler;
		sampler.texture = existing_texture->impl->as<d3d9_tex_data>();
		sampler.states[D3DSAMP_ADDRESSU] = static_cast<D3DTEXTUREADDRESS>(sampler_info.address_u);
		sampler.states[D3DSAMP_ADDRESSV] = static_cast<D3DTEXTUREADDRESS>(sampler_info.address_v);
		sampler.states[D3DSAMP_ADDRESSW] = static_cast<D3DTEXTUREADDRESS>(sampler_info.address_w);
		sampler.states[D3DSAMP_BORDERCOLOR] = 0;
		sampler.states[D3DSAMP_MAGFILTER] = 1 + ((static_cast<unsigned int>(sampler_info.filter) & 0x0C) >> 2);
		sampler.states[D3DSAMP_MINFILTER] = 1 + ((static_cast<unsigned int>(sampler_info.filter) & 0x30) >> 4);
		sampler.states[D3DSAMP_MIPFILTER] = 1 + ((static_cast<unsigned int>(sampler_info.filter) & 0x03));
		sampler.states[D3DSAMP_MIPMAPLODBIAS] = *reinterpret_cast<const DWORD *>(&sampler_info.lod_bias);
		sampler.states[D3DSAMP_MAXMIPLEVEL] = static_cast<DWORD>(std::max(0.0f, sampler_info.min_lod));
		sampler.states[D3DSAMP_MAXANISOTROPY] = 1;
		sampler.states[D3DSAMP_SRGBTEXTURE] = sampler_info.srgb;

		_sampler_bindings.resize(std::max(_sampler_bindings.size(), size_t(sampler_info.binding + 1)));

		_sampler_bindings[sampler_info.binding] = std::move(sampler);
	}

	void d3d9_effect_compiler::visit_uniform(const uniform_info &uniform_info)
	{
		uniform obj;
		obj.name = uniform_info.name;
		obj.rows = uniform_info.type.rows;
		obj.columns = uniform_info.type.cols;
		obj.elements = std::max(1, uniform_info.type.array_length);
		obj.storage_size = uniform_info.size;
		obj.storage_offset = _uniform_storage_offset + uniform_info.offset * 4;
		copy_annotations(uniform_info.annotations, obj.annotations);

		obj.basetype = uniform_datatype::floating_point;

		switch (uniform_info.type.base)
		{
		case type::t_int:
			obj.displaytype = uniform_datatype::signed_integer;
			break;
		case type::t_uint:
			obj.displaytype = uniform_datatype::unsigned_integer;
			break;
		case type::t_float:
			obj.displaytype = uniform_datatype::floating_point;
			break;
		}

		_constant_register_count += obj.storage_size / 4;
		//_constant_register_count += (obj.storage_size / 4 + 4 - ((obj.storage_size / 4) % 4)) / 4;

		auto &uniform_storage = _runtime->get_uniform_value_storage();

		if (obj.storage_offset + obj.storage_size >= uniform_storage.size())
		{
			uniform_storage.resize(uniform_storage.size() + 128);
		}

		if (uniform_info.has_initializer_value)
		{
			for (size_t i = 0; i < obj.storage_size / 4; i++)
			{
				switch (uniform_info.type.base)
				{
				case type::t_int:
					reinterpret_cast<float *>(uniform_storage.data() + obj.storage_offset)[i] = static_cast<float>(uniform_info.initializer_value.as_int[i]);
					break;
				case type::t_uint:
					reinterpret_cast<float *>(uniform_storage.data() + obj.storage_offset)[i] = static_cast<float>(uniform_info.initializer_value.as_uint[i]);
					break;
				case type::t_float:
					reinterpret_cast<float *>(uniform_storage.data() + obj.storage_offset)[i] = uniform_info.initializer_value.as_float[i];
					break;
				}
			}
		}
		else
		{
			memset(uniform_storage.data() + obj.storage_offset, 0, obj.storage_size);
		}

		_runtime->add_uniform(std::move(obj));
	}

	void d3d9_effect_compiler::visit_technique(const technique_info &technique_info)
	{
		technique obj;
		obj.name = technique_info.name;
		copy_annotations(technique_info.annotations, obj.annotations);

		if (_constant_register_count != 0)
		{
			obj.uniform_storage_index = _constant_register_count;
			obj.uniform_storage_offset = _uniform_storage_offset;
		}

		size_t pass_index = 0;

		for (const auto &pass_info : technique_info.passes)
		{
			auto &pass = static_cast<d3d9_pass_data &>(*obj.passes.emplace_back(std::make_unique<d3d9_pass_data>()));

			pass.vertex_shader = vs_entry_points[pass_info.vs_entry_point];
			assert(pass.vertex_shader != nullptr);
			pass.pixel_shader = ps_entry_points[pass_info.ps_entry_point];
			assert(pass.pixel_shader != nullptr);

			pass.sampler_count = std::min<DWORD>(16, DWORD(_sampler_bindings.size()));
			for (size_t i = 0; i < pass.sampler_count; ++i)
				pass.samplers[i] = _sampler_bindings[i];

			pass.render_targets[0] = _runtime->_backbuffer_resolved.get();
			pass.clear_render_targets = pass_info.clear_render_targets;

			const auto &device = _runtime->_device;

			const HRESULT hr = device->BeginStateBlock();

			if (FAILED(hr))
			{
				error("internal pass stateblock creation failed with error code " + std::to_string(static_cast<unsigned long>(hr)) + "!");
				return;
			}

			device->SetVertexShader(pass.vertex_shader.get());
			device->SetPixelShader(pass.pixel_shader.get());

			device->SetRenderState(D3DRS_ZENABLE, false);
			device->SetRenderState(D3DRS_SPECULARENABLE, false);
			device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
			device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
			device->SetRenderState(D3DRS_ZWRITEENABLE, true);
			device->SetRenderState(D3DRS_ALPHATESTENABLE, false);
			device->SetRenderState(D3DRS_LASTPIXEL, true);
			device->SetRenderState(D3DRS_SRCBLEND, literal_to_blend_func(pass_info.src_blend));
			device->SetRenderState(D3DRS_DESTBLEND, literal_to_blend_func(pass_info.dest_blend));
			device->SetRenderState(D3DRS_ALPHAREF, 0);
			device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_ALWAYS);
			device->SetRenderState(D3DRS_DITHERENABLE, false);
			device->SetRenderState(D3DRS_FOGSTART, 0);
			device->SetRenderState(D3DRS_FOGEND, 1);
			device->SetRenderState(D3DRS_FOGDENSITY, 1);
			device->SetRenderState(D3DRS_ALPHABLENDENABLE, pass_info.blend_enable);
			device->SetRenderState(D3DRS_DEPTHBIAS, 0);
			device->SetRenderState(D3DRS_STENCILENABLE, pass_info.stencil_enable);
			device->SetRenderState(D3DRS_STENCILPASS, literal_to_stencil_op(pass_info.stencil_op_pass));
			device->SetRenderState(D3DRS_STENCILFAIL, literal_to_stencil_op(pass_info.stencil_op_fail));
			device->SetRenderState(D3DRS_STENCILZFAIL, literal_to_stencil_op(pass_info.stencil_op_depth_fail));
			device->SetRenderState(D3DRS_STENCILFUNC, static_cast<D3DCMPFUNC>(pass_info.stencil_comparison_func));
			device->SetRenderState(D3DRS_STENCILREF, pass_info.stencil_reference_value);
			device->SetRenderState(D3DRS_STENCILMASK, pass_info.stencil_read_mask);
			device->SetRenderState(D3DRS_STENCILWRITEMASK, pass_info.stencil_write_mask);
			device->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFF);
			device->SetRenderState(D3DRS_LOCALVIEWER, true);
			device->SetRenderState(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
			device->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
			device->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
			device->SetRenderState(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_COLOR2);
			device->SetRenderState(D3DRS_COLORWRITEENABLE, pass_info.color_write_mask);
			device->SetRenderState(D3DRS_BLENDOP, static_cast<D3DBLENDOP>(pass_info.blend_op));
			device->SetRenderState(D3DRS_SCISSORTESTENABLE, false);
			device->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
			device->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, false);
			device->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, false);
			device->SetRenderState(D3DRS_CCW_STENCILFAIL, D3DSTENCILOP_KEEP);
			device->SetRenderState(D3DRS_CCW_STENCILZFAIL, D3DSTENCILOP_KEEP);
			device->SetRenderState(D3DRS_CCW_STENCILPASS, D3DSTENCILOP_KEEP);
			device->SetRenderState(D3DRS_CCW_STENCILFUNC, D3DCMP_ALWAYS);
			device->SetRenderState(D3DRS_COLORWRITEENABLE1, 0x0000000F);
			device->SetRenderState(D3DRS_COLORWRITEENABLE2, 0x0000000F);
			device->SetRenderState(D3DRS_COLORWRITEENABLE3, 0x0000000F);
			device->SetRenderState(D3DRS_BLENDFACTOR, 0xFFFFFFFF);
			device->SetRenderState(D3DRS_SRGBWRITEENABLE, pass_info.srgb_write_enable);
			device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, false);
			device->SetRenderState(D3DRS_SRCBLENDALPHA, literal_to_blend_func(pass_info.src_blend_alpha));
			device->SetRenderState(D3DRS_DESTBLENDALPHA, literal_to_blend_func(pass_info.dest_blend_alpha));
			device->SetRenderState(D3DRS_BLENDOPALPHA, static_cast<D3DBLENDOP>(pass_info.blend_op_alpha));
			device->SetRenderState(D3DRS_FOGENABLE, false);
			device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
			device->SetRenderState(D3DRS_LIGHTING, false);

			device->EndStateBlock(&pass.stateblock);

			D3DCAPS9 caps;
			device->GetDeviceCaps(&caps);

			for (unsigned int i = 0; i < 8; ++i)
			{
				const std::string &render_target = pass_info.render_target_names[i];

				if (render_target.empty())
					continue;

				const auto texture = _runtime->find_texture(render_target);

				if (texture == nullptr)
				{
					error("texture not found");
					return;
				}

				if (i > caps.NumSimultaneousRTs)
				{
					warning("device only supports " + std::to_string(caps.NumSimultaneousRTs) + " simultaneous render targets, but pass '" + std::to_string(pass_index) + "' uses more, which are ignored");
					break;
				}

				// Unset textures that are used as render target
				for (size_t s = 0; s < pass.sampler_count; ++s)
				{
					if (pass.samplers[s].texture == texture->impl->as<d3d9_tex_data>())
						pass.samplers[s].texture = nullptr;
				}

				pass.render_targets[i] = texture->impl->as<d3d9_tex_data>()->surface.get();
			}

			++pass_index;
		}

		_runtime->add_technique(std::move(obj));
	}

	void d3d9_effect_compiler::compile_entry_point(const std::string &entry_point, bool is_ps)
	{
		// Compile the generated HLSL source code to DX byte code
		com_ptr<ID3DBlob> compiled, errors;

		const auto D3DCompile = reinterpret_cast<pD3DCompile>(GetProcAddress(_d3dcompiler_module, "D3DCompile"));

		HRESULT hr = D3DCompile(_module->hlsl.c_str(), _module->hlsl.size(), nullptr, nullptr, nullptr, entry_point.c_str(), is_ps ? "ps_3_0" : "vs_3_0", 0, 0, &compiled, &errors);

		if (errors != nullptr)
			_errors.append(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize() - 1); // Subtracting one to not append the null-terminator as well

		if (FAILED(hr))
		{
			error("internal shader compilation failed");
			return;
		}

		// Create runtime shader objects from the compiled DX byte code
		if (is_ps)
			hr = _runtime->_device->CreatePixelShader(static_cast<const DWORD *>(compiled->GetBufferPointer()), &ps_entry_points[entry_point]);
		else
			hr = _runtime->_device->CreateVertexShader(static_cast<const DWORD *>(compiled->GetBufferPointer()), &vs_entry_points[entry_point]);

		if (FAILED(hr))
		{
			error("internal shader creation failed with error code " + std::to_string(static_cast<unsigned long>(hr)) + "!");
			return;
		}
	}
}
