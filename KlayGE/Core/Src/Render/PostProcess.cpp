// PostProcess.cpp
// KlayGE 后期处理类 实现文件
// Ver 3.10.0
// 版权所有(C) 龚敏敏, 2006-2010
// Homepage: http://www.klayge.org
//
// 3.10.0
// 使用InputPin和OutputPin来指定输入输出 (2010.3.23)
//
// 3.6.0
// 增加了BlurPostProcess (2007.3.24)
//
// 3.5.0
// 增加了GammaCorrectionProcess (2007.1.22)
//
// 3.3.0
// 初次建立 (2006.6.23)
//
// 修改记录
//////////////////////////////////////////////////////////////////////////////////

#include <KlayGE/KlayGE.hpp>
#include <KlayGE/Context.hpp>
#include <KFL/Math.hpp>
#include <KFL/Util.hpp>
#include <KlayGE/RenderFactory.hpp>
#include <KlayGE/RenderEngine.hpp>
#include <KlayGE/RenderEffect.hpp>
#include <KlayGE/RenderableHelper.hpp>
#include <KlayGE/FrameBuffer.hpp>
#include <KlayGE/RenderLayout.hpp>
#include <KFL/XMLDom.hpp>
#include <KlayGE/ResLoader.hpp>
#include <KlayGE/Camera.hpp>
#include <KFL/Hash.hpp>

#include <cstring>
#include <mutex>

#include <KlayGE/PostProcess.hpp>

namespace
{
	using namespace KlayGE;

	class PostProcessLoadingDesc : public ResLoadingDesc
	{
	private:
		struct PostProcessDesc
		{
			std::string res_name;
			std::string pp_name;

			struct PostProcessData
			{
				std::wstring name;
				bool volumetric;
				std::vector<std::string> param_names;
				std::vector<std::string> input_pin_names;
				std::vector<std::string> output_pin_names;
				uint32_t cs_data_per_thread_x;
				uint32_t cs_data_per_thread_y;
				uint32_t cs_data_per_thread_z;
				std::string effect_name;
				std::string tech_name;
			};
			std::shared_ptr<PostProcessData> pp_data;

			std::shared_ptr<PostProcessPtr> pp;
		};

	public:
		PostProcessLoadingDesc(std::string_view res_name, std::string_view pp_name)
		{
			pp_desc_.res_name = std::string(res_name);
			pp_desc_.pp_name = std::string(pp_name);
			pp_desc_.pp_data = MakeSharedPtr<PostProcessDesc::PostProcessData>();
			pp_desc_.pp = MakeSharedPtr<PostProcessPtr>();
		}

		uint64_t Type() const override
		{
			static uint64_t const type = CT_HASH("PostProcessLoadingDesc");
			return type;
		}

		bool StateLess() const override
		{
			return false;
		}

		void SubThreadStage() override
		{
			std::lock_guard<std::mutex> lock(main_thread_stage_mutex_);
			if (*pp_desc_.pp)
			{
				return;
			}

			ResIdentifierPtr ppmm_input = Context::Instance().ResLoaderInstance().Open(pp_desc_.res_name);

			KlayGE::XMLNode root = LoadXml(*ppmm_input);

			pp_desc_.pp_data->cs_data_per_thread_x = 1;
			pp_desc_.pp_data->cs_data_per_thread_y = 1;
			pp_desc_.pp_data->cs_data_per_thread_z = 1;

			for (XMLNode const* pp_node = root.FirstNode("post_processor"); pp_node; pp_node = pp_node->NextSibling("post_processor"))
			{
				std::string_view name = pp_node->Attrib("name")->ValueString();
				if (pp_desc_.pp_name == name)
				{
					Convert(pp_desc_.pp_data->name, name);

					if (XMLAttribute const* vol_attr = pp_node->Attrib("volumetric"))
					{
						pp_desc_.pp_data->volumetric = (vol_attr->ValueInt() != 0);
					}
					else
					{
						pp_desc_.pp_data->volumetric = false;
					}

					if (XMLNode const* params_chunk = pp_node->FirstNode("params"))
					{
						for (XMLNode const* p_node = params_chunk->FirstNode("param"); p_node; p_node = p_node->NextSibling("param"))
						{
							pp_desc_.pp_data->param_names.emplace_back(p_node->Attrib("name")->ValueString());
						}
					}
					if (XMLNode const* input_chunk = pp_node->FirstNode("input"))
					{
						for (XMLNode const* pin_node = input_chunk->FirstNode("pin"); pin_node;
							 pin_node = pin_node->NextSibling("pin"))
						{
							pp_desc_.pp_data->input_pin_names.emplace_back(pin_node->Attrib("name")->ValueString());
						}
					}
					if (XMLNode const* output_chunk = pp_node->FirstNode("output"))
					{
						for (XMLNode const* pin_node = output_chunk->FirstNode("pin"); pin_node;
							 pin_node = pin_node->NextSibling("pin"))
						{
							pp_desc_.pp_data->output_pin_names.emplace_back(pin_node->Attrib("name")->ValueString());
						}
					}
					if (XMLNode const* shader_chunk = pp_node->FirstNode("shader"))
					{
						pp_desc_.pp_data->effect_name = std::string(shader_chunk->Attrib("effect")->ValueString());
						pp_desc_.pp_data->tech_name = std::string(shader_chunk->Attrib("tech")->ValueString());

						if (XMLAttribute const* attr = shader_chunk->Attrib("cs_data_per_thread_x"))
						{
							pp_desc_.pp_data->cs_data_per_thread_x = attr->ValueUInt();
						}
						if (XMLAttribute const* attr = shader_chunk->Attrib("cs_data_per_thread_y"))
						{
							pp_desc_.pp_data->cs_data_per_thread_y = attr->ValueUInt();
						}
						if (XMLAttribute const* attr = shader_chunk->Attrib("cs_data_per_thread_z"))
						{
							pp_desc_.pp_data->cs_data_per_thread_z = attr->ValueUInt();
						}
					}
				}
			}

			RenderFactory& rf = Context::Instance().RenderFactoryInstance();
			RenderDeviceCaps const & caps = rf.RenderEngineInstance().DeviceCaps();
			if (caps.multithread_res_creating_support)
			{
				this->MainThreadStageNoLock();
			}
		}

		void MainThreadStage() override
		{
			std::lock_guard<std::mutex> lock(main_thread_stage_mutex_);
			this->MainThreadStageNoLock();
		}

		bool HasSubThreadStage() const override
		{
			return true;
		}

		bool Match(ResLoadingDesc const & rhs) const override
		{
			if (this->Type() == rhs.Type())
			{
				PostProcessLoadingDesc const & ppld = static_cast<PostProcessLoadingDesc const &>(rhs);
				return (pp_desc_.res_name == ppld.pp_desc_.res_name)
					&& (pp_desc_.pp_name == ppld.pp_desc_.pp_name);
			}
			return false;
		}

		void CopyDataFrom(ResLoadingDesc const & rhs) override
		{
			BOOST_ASSERT(this->Type() == rhs.Type());

			PostProcessLoadingDesc const & ppld = static_cast<PostProcessLoadingDesc const &>(rhs);
			pp_desc_.res_name = ppld.pp_desc_.res_name;
			pp_desc_.pp_name = ppld.pp_desc_.pp_name;
			pp_desc_.pp_data = ppld.pp_desc_.pp_data;
		}

		std::shared_ptr<void> CloneResourceFrom(std::shared_ptr<void> const & resource) override
		{
			PostProcessPtr rhs_pp = std::static_pointer_cast<PostProcess>(resource);
			return std::static_pointer_cast<void>(rhs_pp->Clone());
		}

		std::shared_ptr<void> Resource() const override
		{
			return *pp_desc_.pp;
		}

	private:
		void MainThreadStageNoLock()
		{
			if (!*pp_desc_.pp)
			{
				auto effect = SyncLoadRenderEffect(pp_desc_.pp_data->effect_name);
				auto tech = effect->TechniqueByName(pp_desc_.pp_data->tech_name);

				PostProcessPtr pp = MakeSharedPtr<PostProcess>(pp_desc_.pp_data->name, pp_desc_.pp_data->volumetric,
					pp_desc_.pp_data->param_names,
					pp_desc_.pp_data->input_pin_names, pp_desc_.pp_data->output_pin_names, effect, tech);
				pp->CSPixelPerThreadX(pp_desc_.pp_data->cs_data_per_thread_x);
				pp->CSPixelPerThreadY(pp_desc_.pp_data->cs_data_per_thread_y);
				pp->CSPixelPerThreadZ(pp_desc_.pp_data->cs_data_per_thread_z);
				*pp_desc_.pp = pp;
			}
		}

	private:
		PostProcessDesc pp_desc_;
		std::mutex main_thread_stage_mutex_;
	};
}

namespace KlayGE
{
	PostProcess::PostProcess(std::wstring_view name, bool volumetric)
			: Renderable(name),
				volumetric_(volumetric),
				cs_based_(false), cs_pixel_per_thread_x_(1), cs_pixel_per_thread_y_(1), cs_pixel_per_thread_z_(1),
				num_bind_output_(0)
	{
	}

	PostProcess::PostProcess(std::wstring_view name, bool volumetric,
		std::span<std::string const> param_names,
		std::span<std::string const> input_pin_names,
		std::span<std::string const> output_pin_names,
		RenderEffectPtr const & effect, RenderTechnique* tech)
			: PostProcess(name, volumetric)
	{
		input_pins_.resize(input_pin_names.size());
		for (size_t i = 0; i < input_pin_names.size(); ++ i)
		{
			input_pins_[i].first = input_pin_names[i];
		}
		output_pins_.resize(output_pin_names.size());
		for (size_t i = 0; i < output_pin_names.size(); ++i)
		{
			std::get<0>(output_pins_[i]) = output_pin_names[i];
		}
		params_.resize(param_names.size());
		for (size_t i = 0; i < param_names.size(); ++i)
		{
			params_[i].first = param_names[i];
		}
		input_pins_ep_.resize(input_pin_names.size());
		this->Technique(effect, tech);
	}

	PostProcess::~PostProcess() noexcept = default;

	PostProcessPtr PostProcess::Clone()
	{
		RenderEffectPtr effect = effect_->Clone();
		RenderTechnique* tech = effect->TechniqueByName(technique_->Name());

		std::vector<std::string> param_names(params_.size());
		for (size_t i = 0; i < param_names.size(); ++ i)
		{
			param_names[i] = params_[i].first;
		}

		std::vector<std::string> input_pin_names(input_pins_.size());
		for (size_t i = 0; i < input_pin_names.size(); ++ i)
		{
			input_pin_names[i] = input_pins_[i].first;
		}

		std::vector<std::string> output_pin_names(output_pins_.size());
		for (size_t i = 0; i < output_pin_names.size(); ++ i)
		{
			output_pin_names[i] = std::get<0>(output_pins_[i]);
		}

		PostProcessPtr pp = MakeSharedPtr<PostProcess>(this->Name(), volumetric_,
			param_names, input_pin_names, output_pin_names, effect, tech);
		pp->CSPixelPerThreadX(cs_pixel_per_thread_x_);
		pp->CSPixelPerThreadY(cs_pixel_per_thread_y_);
		pp->CSPixelPerThreadZ(cs_pixel_per_thread_z_);
		return pp;
	}

	void PostProcess::Technique(RenderEffectPtr const & effect, RenderTechnique* tech)
	{
		effect_ = effect;
		technique_ = tech;
		this->UpdateBinds();

		if (technique_)
		{
			BOOST_ASSERT(effect);

			cs_based_ = !!technique_->Pass(0).GetShaderObject(*effect_)->Stage(ShaderStage::Compute);

			BOOST_ASSERT(!(cs_based_ && volumetric_));

			if (volumetric_)
			{
				pp_mvp_param_ = effect_->ParameterByName("pp_mvp");
			}
			else
			{
				pp_mvp_param_ = nullptr;
			}
		}
		else
		{
			cs_based_ = false;
		}

		cs_pixel_per_thread_x_ = 1;
		cs_pixel_per_thread_y_ = 1;
		cs_pixel_per_thread_z_ = 1;

		this->CreateVB();
	}

	void PostProcess::UpdateBinds()
	{
		if (technique_)
		{
			BOOST_ASSERT(effect_);

			input_pins_ep_.resize(input_pins_.size());
			for (size_t i = 0; i < input_pins_.size(); ++ i)
			{
				input_pins_ep_[i] = effect_->ParameterByName(input_pins_[i].first);
			}

			output_pins_ep_.resize(output_pins_.size());
			for (size_t i = 0; i < output_pins_.size(); ++ i)
			{
				output_pins_ep_[i] = effect_->ParameterByName(std::get<0>(output_pins_[i]));
			}

			for (size_t i = 0; i < params_.size(); ++ i)
			{
				params_[i].second = effect_->ParameterByName(params_[i].first);
			}

			width_height_ep_ = effect_->ParameterByName("width_height");
			inv_width_height_ep_ = effect_->ParameterByName("inv_width_height");
		}
	}

	uint32_t PostProcess::NumParams() const
	{
		return static_cast<uint32_t>(params_.size());
	}

	uint32_t PostProcess::ParamByName(std::string_view name) const
	{
		for (size_t i = 0; i < params_.size(); ++ i)
		{
			if (params_[i].first == name)
			{
				return static_cast<uint32_t>(i);
			}
		}
		return 0xFFFFFFFF;
	}

	std::string const & PostProcess::ParamName(uint32_t index) const
	{
		return params_[index].first;
	}

	void PostProcess::SetParam(uint32_t index, bool const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, uint32_t const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, int32_t const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, float const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, uint2 const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, uint3 const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, uint4 const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, int2 const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, int3 const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, int4 const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, float2 const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, float3 const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, float4 const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, float4x4 const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, std::vector<bool> const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, std::vector<uint32_t> const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, std::vector<int32_t> const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, std::vector<float> const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, std::vector<uint2> const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, std::vector<uint3> const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, std::vector<uint4> const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, std::vector<int2> const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, std::vector<int3> const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, std::vector<int4> const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, std::vector<float2> const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, std::vector<float3> const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, std::vector<float4> const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::SetParam(uint32_t index, std::vector<float4x4> const & value)
	{
		*params_[index].second = value;
	}

	void PostProcess::GetParam(uint32_t index, bool& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, uint32_t& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, int32_t& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, float& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, uint2& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, uint3& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, uint4& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, int2& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, int3& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, int4& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, float2& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, float3& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, float4& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, float4x4& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, std::vector<bool>& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, std::vector<uint32_t>& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, std::vector<int32_t>& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, std::vector<float>& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, std::vector<uint2>& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, std::vector<uint3>& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, std::vector<uint4>& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, std::vector<int2>& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, std::vector<int3>& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, std::vector<int4>& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, std::vector<float2>& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, std::vector<float3>& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, std::vector<float4>& value)
	{
		params_[index].second->Value(value);
	}

	void PostProcess::GetParam(uint32_t index, std::vector<float4x4>& value)
	{
		params_[index].second->Value(value);
	}

	uint32_t PostProcess::NumInputPins() const
	{
		return static_cast<uint32_t>(input_pins_.size());
	}

	uint32_t PostProcess::InputPinByName(std::string_view name) const
	{
		for (size_t i = 0; i < input_pins_.size(); ++ i)
		{
			if (input_pins_[i].first == name)
			{
				return static_cast<uint32_t>(i);
			}
		}
		return 0xFFFFFFFF;
	}

	std::string const & PostProcess::InputPinName(uint32_t index) const
	{
		return input_pins_[index].first;
	}

	void PostProcess::InputPin(uint32_t index, ShaderResourceViewPtr const & srv)
	{
		input_pins_[index].second = srv;
		*(input_pins_ep_[index]) = srv;

		if ((0 == index) && srv)
		{
			auto const & tex = srv->TextureResource();
			float const width = static_cast<float>(tex->Width(0));
			float const height = static_cast<float>(tex->Height(0));
			if (width_height_ep_)
			{
				*width_height_ep_ = float4(width, height, 1 / width, 1 / height);
			}
			if (inv_width_height_ep_)
			{
				*inv_width_height_ep_ = float2(1 / width, 1 / height);
			}
		}
	}

	ShaderResourceViewPtr const & PostProcess::InputPin(uint32_t index) const
	{
		BOOST_ASSERT(index < input_pins_.size());
		return input_pins_[index].second;
	}

	uint32_t PostProcess::NumOutputPins() const
	{
		return static_cast<uint32_t>(output_pins_.size());
	}

	uint32_t PostProcess::OutputPinByName(std::string_view name) const
	{
		for (size_t i = 0; i < output_pins_.size(); ++ i)
		{
			if (std::get<0>(output_pins_[i]) == name)
			{
				return static_cast<uint32_t>(i);
			}
		}
		return 0xFFFFFFFF;
	}

	std::string const & PostProcess::OutputPinName(uint32_t index) const
	{
		return std::get<0>(output_pins_[index]);
	}

	void PostProcess::OutputPin(uint32_t index, RenderTargetViewPtr const& rtv)
	{
		if ((!std::get<1>(output_pins_[index]) && !std::get<2>(output_pins_[index])) && rtv)
		{
			++num_bind_output_;
		}
		if ((std::get<1>(output_pins_[index]) || std::get<2>(output_pins_[index])) && !rtv)
		{
			--num_bind_output_;
		}

		if (rtv)
		{
			frame_buffer_->Attach(FrameBuffer::CalcAttachment(index), rtv);
		}
		
		std::get<1>(output_pins_[index]) = rtv;
		std::get<2>(output_pins_[index]) = UnorderedAccessViewPtr();
	}

	void PostProcess::OutputPin(uint32_t index, UnorderedAccessViewPtr const& uav)
	{
		if ((!std::get<1>(output_pins_[index]) && !std::get<2>(output_pins_[index])) && uav)
		{
			++num_bind_output_;
		}
		if ((std::get<1>(output_pins_[index]) || std::get<2>(output_pins_[index])) && !uav)
		{
			--num_bind_output_;
		}

		if (output_pins_ep_[index])
		{
			*(output_pins_ep_[index]) = uav;
		}

		std::get<1>(output_pins_[index]) = RenderTargetViewPtr();
		std::get<2>(output_pins_[index]) = uav;
	}

	RenderTargetViewPtr const & PostProcess::RtvOutputPin(uint32_t index) const
	{
		BOOST_ASSERT(index < output_pins_.size());
		return std::get<1>(output_pins_[index]);
	}

	UnorderedAccessViewPtr const & PostProcess::UavOutputPin(uint32_t index) const
	{
		BOOST_ASSERT(index < output_pins_.size());
		return std::get<2>(output_pins_[index]);
	}

	void PostProcess::Apply()
	{
		RenderEngine& re = Context::Instance().RenderFactoryInstance().RenderEngineInstance();
		if (cs_based_)
		{
			re.BindFrameBuffer(re.DefaultFrameBuffer());
			re.DefaultFrameBuffer()->Discard(FrameBuffer::CBM_Color);

			auto const& cs_stage = technique_->Pass(0).GetShaderObject(*effect_)->Stage(ShaderStage::Compute);
			uint32_t const bx = cs_stage->BlockSizeX() * cs_pixel_per_thread_x_;
			uint32_t const by = cs_stage->BlockSizeY() * cs_pixel_per_thread_y_;
			uint32_t const bz = cs_stage->BlockSizeZ() * cs_pixel_per_thread_z_;
			
			BOOST_ASSERT(bx > 0);
			BOOST_ASSERT(by > 0);
			BOOST_ASSERT(bz > 0);

			auto const* tex = this->UavOutputPin(0)->TextureResource().get();
			uint32_t tgx = (tex->Width(0) + bx - 1) / bx;
			uint32_t tgy = (tex->Height(0) + by - 1) / by;
			uint32_t tgz = (tex->Depth(0) + bz - 1) / bz;

			this->OnRenderBegin();
			re.Dispatch(*effect_, *technique_, tgx, tgy, tgz);
			this->OnRenderEnd();
		}
		else
		{
			FrameBufferPtr const & fb = (0 == num_bind_output_) ? re.DefaultFrameBuffer() : frame_buffer_;
			re.BindFrameBuffer(fb);
			if (volumetric_)
			{
				BOOST_ASSERT(pp_mvp_param_);

				*pp_mvp_param_ = model_mat_ * frame_buffer_->Viewport()->Camera()->ViewProjMatrix();
			}
			else
			{
				ViewportPtr const& vp = fb->Viewport();
				if ((!technique_->Transparent()) && (0 == vp->Left()) && (0 == vp->Top())
					&& (fb->Width() == static_cast<uint32_t>(vp->Width()))
					&& (fb->Height() == static_cast<uint32_t>(vp->Height())))
				{
					fb->Discard(FrameBuffer::CBM_Color);
				}
			}
			this->Render();
		}
	}

	void PostProcess::OnRenderBegin()
	{
	}

	void PostProcess::CreateVB()
	{
		if (cs_based_)
		{
			rls_[0].reset();
			frame_buffer_.reset();
		}
		else
		{
			if (!rls_[0])
			{
				RenderFactory& rf = Context::Instance().RenderFactoryInstance();
				RenderEngine& re = rf.RenderEngineInstance();

				if (volumetric_)
				{
					rls_[0] = re.VolumetricPostProcessRenderLayout();
				}
				else
				{
					rls_[0] = re.PostProcessRenderLayout();
				}

				pos_aabb_ = AABBox(float3(-1, -1, -1), float3(1, 1, 1));
				tc_aabb_ = AABBox(float3(0, 0, 0), float3(1, 1, 0));

				frame_buffer_ = rf.MakeFrameBuffer();
				frame_buffer_->Viewport()->Camera(rf.RenderEngineInstance().CurFrameBuffer()->Viewport()->Camera());
			}
		}
	}


	PostProcessPtr SyncLoadPostProcess(std::string_view ppml_name, std::string_view pp_name)
	{
		return Context::Instance().ResLoaderInstance().SyncQueryT<PostProcess>(MakeSharedPtr<PostProcessLoadingDesc>(ppml_name, pp_name));
	}

	PostProcessPtr ASyncLoadPostProcess(std::string_view ppml_name, std::string_view pp_name)
	{
		// TODO: Make it really async
		return Context::Instance().ResLoaderInstance().SyncQueryT<PostProcess>(MakeSharedPtr<PostProcessLoadingDesc>(ppml_name, pp_name));
	}


	PostProcessChain::PostProcessChain(std::wstring const & name)
			: PostProcess(name, false)
	{
	}

	PostProcessChain::PostProcessChain(std::wstring const & name,
		std::span<std::string const> param_names,
		std::span<std::string const> input_pin_names,
		std::span<std::string const> output_pin_names,
		RenderEffectPtr const & effect, RenderTechnique* tech)
			: PostProcess(name, false, param_names, input_pin_names, output_pin_names, effect, tech)
	{
	}

	void PostProcessChain::Append(PostProcessPtr const & pp)
	{
		pp_chain_.push_back(pp);
	}

	uint32_t PostProcessChain::NumPostProcesses() const
	{
		return static_cast<uint32_t>(pp_chain_.size());
	}

	PostProcessPtr const & PostProcessChain::GetPostProcess(uint32_t index) const
	{
		BOOST_ASSERT(index < pp_chain_.size());
		return pp_chain_[index];
	}

	uint32_t PostProcessChain::NumParams() const
	{
		return pp_chain_.front()->NumParams();
	}

	uint32_t PostProcessChain::ParamByName(std::string_view name) const
	{
		return pp_chain_.front()->ParamByName(name);
	}

	std::string const & PostProcessChain::ParamName(uint32_t index) const
	{
		return pp_chain_.front()->ParamName(index);
	}

	void PostProcessChain::SetParam(uint32_t index, bool const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, uint32_t const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, int32_t const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, float const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, uint2 const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, uint3 const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, uint4 const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, int2 const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, int3 const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, int4 const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, float2 const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, float3 const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, float4 const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, float4x4 const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, std::vector<bool> const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, std::vector<uint32_t> const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, std::vector<int32_t> const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, std::vector<float> const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, std::vector<uint2> const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, std::vector<uint3> const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, std::vector<uint4> const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, std::vector<int2> const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, std::vector<int3> const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, std::vector<int4> const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, std::vector<float2> const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, std::vector<float3> const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, std::vector<float4> const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::SetParam(uint32_t index, std::vector<float4x4> const & value)
	{
		pp_chain_.front()->SetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, bool& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, uint32_t& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, int32_t& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, float& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, uint2& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, uint3& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, uint4& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, int2& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, int3& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, int4& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, float2& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, float3& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, float4& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, float4x4& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, std::vector<bool>& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, std::vector<uint32_t>& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, std::vector<int32_t>& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, std::vector<float>& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, std::vector<uint2>& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, std::vector<uint3>& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, std::vector<uint4>& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, std::vector<int2>& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, std::vector<int3>& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, std::vector<int4>& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, std::vector<float2>& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, std::vector<float3>& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, std::vector<float4>& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	void PostProcessChain::GetParam(uint32_t index, std::vector<float4x4>& value)
	{
		pp_chain_.front()->GetParam(index, value);
	}

	uint32_t PostProcessChain::NumInputPins() const
	{
		return pp_chain_.front()->NumInputPins();
	}

	uint32_t PostProcessChain::InputPinByName(std::string_view name) const
	{
		return pp_chain_.front()->InputPinByName(name);
	}

	std::string const & PostProcessChain::InputPinName(uint32_t index) const
	{
		return pp_chain_.front()->InputPinName(index);
	}

	void PostProcessChain::InputPin(uint32_t index, ShaderResourceViewPtr const& srv)
	{
		pp_chain_.front()->InputPin(index, srv);
	}

	ShaderResourceViewPtr const& PostProcessChain::InputPin(uint32_t index) const
	{
		return pp_chain_.front()->InputPin(index);
	}

	uint32_t PostProcessChain::NumOutputPins() const
	{
		return pp_chain_.back()->NumOutputPins();
	}

	uint32_t PostProcessChain::OutputPinByName(std::string_view name) const
	{
		return pp_chain_.back()->OutputPinByName(name);
	}

	std::string const & PostProcessChain::OutputPinName(uint32_t index) const
	{
		return pp_chain_.back()->OutputPinName(index);
	}

	void PostProcessChain::OutputPin(uint32_t index, RenderTargetViewPtr const& rtv)
	{
		pp_chain_.back()->OutputPin(index, rtv);
	}

	void PostProcessChain::OutputPin(uint32_t index, UnorderedAccessViewPtr const& uav)
	{
		pp_chain_.back()->OutputPin(index, uav);
	}

	RenderTargetViewPtr const& PostProcessChain::RtvOutputPin(uint32_t index) const
	{
		return pp_chain_.back()->RtvOutputPin(index);
	}

	UnorderedAccessViewPtr const& PostProcessChain::UavOutputPin(uint32_t index) const
	{
		return pp_chain_.back()->UavOutputPin(index);
	}

	void PostProcessChain::Apply()
	{
		for (auto const & pp : pp_chain_)
		{
			pp->Apply();
		}
	}


	SeparableBoxFilterPostProcess::SeparableBoxFilterPostProcess(RenderEffectPtr const & effect,
		RenderTechnique* tech, int kernel_radius, float multiplier, bool x_dir)
		: PostProcess(L"SeparableBoxFilter", false,
				MakeSpan<std::string>(),
				MakeSpan<std::string>({"src_tex"}),
				MakeSpan<std::string>({"output"}),
				effect, tech),
			kernel_radius_(kernel_radius), multiplier_(multiplier), x_dir_(x_dir)
	{
		BOOST_ASSERT((kernel_radius > 0) && (kernel_radius <= 8));

		RenderEffectPtr ei;
		RenderTechnique* te;
		if (tech)
		{
			BOOST_ASSERT(effect);

			ei = effect;
			te = tech;
		}
		else
		{
			ei = SyncLoadRenderEffect("Blur.fxml");
			te = ei->TechniqueByName(x_dir ? "BlurX" : "BlurY");
		}
		this->Technique(ei, te);

		src_tex_size_ep_ = effect_->ParameterByName("src_tex_size");
		color_weight_ep_ = effect_->ParameterByName("color_weight");
		tex_coord_offset_ep_ = effect_->ParameterByName("tex_coord_offset");
	}

	void SeparableBoxFilterPostProcess::InputPin(uint32_t index, ShaderResourceViewPtr const& srv)
	{
		PostProcess::InputPin(index, srv);
		if (0 == index)
		{
			auto const* tex = srv->TextureResource().get();
			this->CalSampleOffsets(x_dir_ ? tex->Width(0) : tex->Height(0));
		}
	}

	void SeparableBoxFilterPostProcess::KernelRadius(int radius)
	{
		kernel_radius_ = radius;
		auto const* srv = this->InputPin(0).get();
		if (srv)
		{
			auto const* tex = srv->TextureResource().get();
			this->CalSampleOffsets(x_dir_ ? tex->Width(0) : tex->Height(0));
		}
	}

	void SeparableBoxFilterPostProcess::Multiplier(float multiplier)
	{
		multiplier_ = multiplier;
		auto const* srv = this->InputPin(0).get();
		if (srv)
		{
			auto const* tex = srv->TextureResource().get();
			this->CalSampleOffsets(x_dir_ ? tex->Width(0) : tex->Height(0));
		}
	}

	void SeparableBoxFilterPostProcess::CalSampleOffsets(uint32_t tex_size)
	{
		std::vector<float> color_weight(8, multiplier_ / (2 * kernel_radius_ + 1));
		std::vector<float> tex_coord_offset(8, 0);

		float const tu = 1.0f / tex_size;

		for (int i = 0; i < kernel_radius_; ++ i)
		{
			color_weight[i] *= 2;
			tex_coord_offset[i] = (i * 2 - kernel_radius_ + 0.5f) * tu;
		}
		tex_coord_offset[kernel_radius_] = kernel_radius_ * tu;

		*src_tex_size_ep_ = float2(static_cast<float>(tex_size), 1.0f / tex_size);
		*color_weight_ep_ = color_weight;
		*tex_coord_offset_ep_ = tex_coord_offset;
	}


	SeparableGaussianFilterPostProcess::SeparableGaussianFilterPostProcess(RenderEffectPtr const & effect,
		RenderTechnique* tech, int kernel_radius, float multiplier, bool x_dir)
			: PostProcess(L"SeparableGaussian", false,
					MakeSpan<std::string>(),
					MakeSpan<std::string>({"src_tex"}),
					MakeSpan<std::string>({"output"}),
					effect, tech),
				kernel_radius_(kernel_radius), multiplier_(multiplier), x_dir_(x_dir)
	{
		BOOST_ASSERT((kernel_radius > 0) && (kernel_radius <= 8));

		RenderEffectPtr ei;
		RenderTechnique* te;
		if (tech)
		{
			BOOST_ASSERT(effect);

			ei = effect;
			te = tech;
		}
		else
		{
			ei = SyncLoadRenderEffect("Blur.fxml");
			te = ei->TechniqueByName(x_dir ? "BlurX" : "BlurY");
		}
		this->Technique(ei, te);

		src_tex_size_ep_ = effect_->ParameterByName("src_tex_size");
		color_weight_ep_ = effect_->ParameterByName("color_weight");
		tex_coord_offset_ep_ = effect_->ParameterByName("tex_coord_offset");
	}

	void SeparableGaussianFilterPostProcess::InputPin(uint32_t index, ShaderResourceViewPtr const& srv)
	{
		PostProcess::InputPin(index, srv);
		if (0 == index)
		{
			auto const* tex = srv->TextureResource().get();
			this->CalSampleOffsets(x_dir_ ? tex->Width(0) : tex->Height(0), 3.0f);
		}
	}

	void SeparableGaussianFilterPostProcess::KernelRadius(int radius)
	{
		kernel_radius_ = radius;
		auto const* srv = this->InputPin(0).get();
		if (srv)
		{
			auto const* tex = srv->TextureResource().get();
			this->CalSampleOffsets(x_dir_ ? tex->Width(0) : tex->Height(0), 3.0f);
		}
	}

	void SeparableGaussianFilterPostProcess::Multiplier(float multiplier)
	{
		multiplier_ = multiplier;
		auto const* srv = this->InputPin(0).get();
		if (srv)
		{
			auto const* tex = srv->TextureResource().get();
			this->CalSampleOffsets(x_dir_ ? tex->Width(0) : tex->Height(0), 3.0f);
		}
	}

	float SeparableGaussianFilterPostProcess::GaussianDistribution(float x, float y, float rho)
	{
		float g = 1.0f / sqrt(2.0f * PI * rho * rho);
		g *= exp(-(x * x + y * y) / (2 * rho * rho));
		return g;
	}

	void SeparableGaussianFilterPostProcess::CalSampleOffsets(uint32_t tex_size, float deviation)
	{
		std::vector<float> color_weight(8, 0);
		std::vector<float> tex_coord_offset(8, 0);

		std::vector<float> tmp_weights(kernel_radius_ * 2, 0);
		std::vector<float> tmp_offset(kernel_radius_ * 2, 0);

		float const tu = 1.0f / tex_size;

		float sum_weight = 0;
		for (int i = 0; i < 2 * kernel_radius_; ++ i)
		{
			float weight = this->GaussianDistribution(static_cast<float>(i - kernel_radius_), 0, kernel_radius_ / deviation);
			tmp_weights[i] = weight;
			sum_weight += weight;
		}
		for (int i = 0; i < 2 * kernel_radius_; ++ i)
		{
			tmp_weights[i] /= sum_weight;
		}

		// Fill the offsets
		for (int i = 0; i < kernel_radius_; ++ i)
		{
			tmp_offset[i]                  = static_cast<float>(i - kernel_radius_);
			tmp_offset[i + kernel_radius_] = static_cast<float>(i);
		}

		// Bilinear filtering taps
		// Ordering is left to right.
		for (int i = 0; i < kernel_radius_; ++ i)
		{
			float const scale = tmp_weights[i * 2] + tmp_weights[i * 2 + 1];
			float const frac = tmp_weights[i * 2] / scale;

			tex_coord_offset[i] = (tmp_offset[i * 2] + (1 - frac)) * tu;
			color_weight[i] = multiplier_ * scale;
		}

		*src_tex_size_ep_ = float2(static_cast<float>(tex_size), 1.0f / tex_size);
		*color_weight_ep_ = color_weight;
		*tex_coord_offset_ep_ = tex_coord_offset;
	}


	SeparableBilateralFilterPostProcess::SeparableBilateralFilterPostProcess(RenderEffectPtr const & effect,
			RenderTechnique* tech, int kernel_radius, float multiplier, bool x_dir)
		: PostProcess(L"SeparableBilateral", false,
			MakeSpan<std::string>(),
			MakeSpan<std::string>({"src1_tex", "src2_tex"}),
			MakeSpan<std::string>({"out_tex"}),
			RenderEffectPtr(), nullptr),
			kernel_radius_(kernel_radius), multiplier_(multiplier), x_dir_(x_dir)
	{
		RenderEffectPtr ei;
		RenderTechnique* te;
		if (tech)
		{
			BOOST_ASSERT(effect);

			ei = effect;
			te = tech;
		}
		else
		{
			ei = SyncLoadRenderEffect("BilateralBlur.fxml");
			te = ei->TechniqueByName(x_dir ? "BlurX4" : "BlurY4");
		}
		this->Technique(ei, te);

		kernel_radius_ep_ = effect_->ParameterByName("kernel_radius");
		src_tex_size_ep_ = effect_->ParameterByName("src_tex_size");
		init_g_ep_ = effect_->ParameterByName("init_g");
		blur_factor_ep_ = effect_->ParameterByName("blur_factor");
		sharpness_factor_ep_ = effect_->ParameterByName("sharpness_factor");
	}

	void SeparableBilateralFilterPostProcess::InputPin(uint32_t index, ShaderResourceViewPtr const& srv)
	{
		PostProcess::InputPin(index, srv);
		if (0 == index)
		{
			auto const* tex = srv->TextureResource().get();
			this->CalSampleOffsets(x_dir_ ? tex->Width(0) : tex->Height(0));
		}
	}

	void SeparableBilateralFilterPostProcess::KernelRadius(int radius)
	{
		kernel_radius_ = radius;
		auto const& srv = this->InputPin(0).get();
		if (srv)
		{
			auto const* tex = srv->TextureResource().get();
			this->CalSampleOffsets(x_dir_ ? tex->Width(0) : tex->Height(0));
		}
	}

	void SeparableBilateralFilterPostProcess::Multiplier(float multiplier)
	{
		multiplier_ = multiplier;
		auto const& srv = this->InputPin(0).get();
		if (srv)
		{
			auto const* tex = srv->TextureResource().get();
			this->CalSampleOffsets(x_dir_ ? tex->Width(0) : tex->Height(0));
		}
	}

	void SeparableBilateralFilterPostProcess::CalSampleOffsets(uint32_t tex_size)
	{
		*kernel_radius_ep_ = static_cast<int32_t>(kernel_radius_);
		*src_tex_size_ep_ = float2(static_cast<float>(tex_size), 1.0f / tex_size);
			
		float rho = kernel_radius_ / 4.0f;
		*init_g_ep_ = multiplier_ / std::sqrt(2.0f * PI * rho * rho);
		float f = 1 / (2 * rho * rho);
		*blur_factor_ep_ = f;
		*sharpness_factor_ep_ = f;
	}
	
	
	SeparableLogGaussianFilterPostProcess::SeparableLogGaussianFilterPostProcess(int kernel_radius, bool linear_depth, bool x_dir)
			: PostProcess(L"SeparableLogGaussian", false,
				MakeSpan<std::string>({"esm_scale_factor", "near_q_far"}),
				MakeSpan<std::string>({"src_tex"}),
				MakeSpan<std::string>({"output"}),
				RenderEffectPtr(), nullptr),
				kernel_radius_(kernel_radius), x_dir_(x_dir)
	{
		BOOST_ASSERT((kernel_radius > 0) && (kernel_radius <= 7));

		auto effect = SyncLoadRenderEffect("Blur.fxml");
		this->Technique(effect, effect->TechniqueByName(x_dir ? (linear_depth ? "LogBlurX" : "LogBlurXNLD") : "LogBlurY"));

		color_weight_ep_ = effect_->ParameterByName("color_weight");
		tex_coord_offset_ep_ = effect_->ParameterByName("tex_coord_offset");
	}

	void SeparableLogGaussianFilterPostProcess::InputPin(uint32_t index, ShaderResourceViewPtr const& srv)
	{
		PostProcess::InputPin(index, srv);
		if (0 == index)
		{
			auto const* tex = srv->TextureResource().get();
			this->CalSampleOffsets(x_dir_ ? tex->Width(0) : tex->Height(0), 3.0f);
		}
	}

	void SeparableLogGaussianFilterPostProcess::KernelRadius(int radius)
	{
		BOOST_ASSERT((radius > 0) && (radius <= 7));

		kernel_radius_ = radius;

		auto const* srv = this->InputPin(0).get();
		if (srv)
		{
			auto const* tex = srv->TextureResource().get();
			this->CalSampleOffsets(x_dir_ ? tex->Width(0) : tex->Height(0), 3.0f);
		}
	}

	float SeparableLogGaussianFilterPostProcess::GaussianDistribution(float x, float y, float rho)
	{
		float g = 1.0f / sqrt(2.0f * PI * rho * rho);
		g *= exp(-(x * x + y * y) / (2 * rho * rho));
		return g;
	}

	void SeparableLogGaussianFilterPostProcess::CalSampleOffsets(uint32_t tex_size, float deviation)
	{
		std::vector<float> color_weight(8, 0);
		std::vector<float> tex_coord_offset(8, 0);

		float const tu = 1.0f / tex_size;

		float sum_weight = 0;
		for (int i = 0; i < kernel_radius_; ++ i)
		{
			float weight = this->GaussianDistribution(static_cast<float>(i - kernel_radius_), 0, kernel_radius_ / deviation);
			color_weight[7 - kernel_radius_ + i] = weight;
		}
		{
			float weight = this->GaussianDistribution(0.0f, 0, kernel_radius_ / deviation);
			color_weight[7] = weight;
			sum_weight += weight;
		}
		for (int i = 0; i < kernel_radius_ + 1; ++ i)
		{
			color_weight[i] /= sum_weight;
		}

		// Fill the offsets
		for (int i = 0; i < kernel_radius_ + 1; ++ i)
		{
			tex_coord_offset[7 - kernel_radius_ + i] = (i - kernel_radius_) * tu;
		}

		*color_weight_ep_ = color_weight;
		*tex_coord_offset_ep_ = tex_coord_offset;
	}


	BicubicFilteringPostProcess::BicubicFilteringPostProcess()
		: PostProcessChain(L"BicubicFiltering")
	{
		this->Append(SyncLoadPostProcess("Resizer.ppml", "bicubic_x"));
		this->Append(SyncLoadPostProcess("Resizer.ppml", "bicubic_y"));
	}

	void BicubicFilteringPostProcess::InputPin(uint32_t index, ShaderResourceViewPtr const& srv)
	{
		pp_chain_[0]->InputPin(index, srv);

		if (0 == index)
		{
			RenderFactory& rf = Context::Instance().RenderFactoryInstance();
			RenderEngine& re = Context::Instance().RenderFactoryInstance().RenderEngineInstance();
			FrameBufferPtr const & fb = re.CurFrameBuffer();

			auto const& tex = *srv->TextureResource();
			uint32_t const tex_width = tex.Width(0);
			uint32_t const tex_height = tex.Height(0);

			float const scale_x = static_cast<float>(fb->Width()) / tex_width;
			float const scale_y = static_cast<float>(fb->Height()) / tex_height;

			uint32_t x_width;
			if (scale_x < scale_y)
			{
				x_width = fb->Width();
			}
			else
			{
				x_width = static_cast<uint32_t>(scale_y * tex_width + 0.5f);
			}
			uint32_t x_height = tex.Height(0);

			if (!blur_x_tex_ || (blur_x_tex_->Width(0) != x_width) || (blur_x_tex_->Height(0) != x_height))
			{
				blur_x_tex_ = rf.MakeTexture2D(x_width, x_height, 1, 1, tex.Format(), 1, 0, EAH_GPU_Read | EAH_GPU_Write);
				blur_x_srv_ = rf.MakeTextureSrv(blur_x_tex_);
				blur_x_rtv_ = rf.Make2DRtv(blur_x_tex_, 0, 1, 0);
			}
			pp_chain_[0]->OutputPin(0, blur_x_rtv_);
			pp_chain_[1]->InputPin(0, blur_x_srv_);
		}
		else
		{
			pp_chain_[1]->InputPin(index, srv);
		}
	}

	void BicubicFilteringPostProcess::SetParam(uint32_t index, float2 const & value)
	{
		pp_chain_[0]->SetParam(index, float2(1, 1));
		pp_chain_[1]->SetParam(index, value);
	}


	LogGaussianBlurPostProcess::LogGaussianBlurPostProcess(int kernel_radius, bool linear_depth)
		: PostProcessChain(L"LogGaussianBlur")
	{
		this->Append(MakeSharedPtr<SeparableLogGaussianFilterPostProcess>(kernel_radius, linear_depth, true));
		this->Append(MakeSharedPtr<SeparableLogGaussianFilterPostProcess>(kernel_radius, linear_depth, false));
	}

	void LogGaussianBlurPostProcess::ESMScaleFactor(float factor, Camera const & camera)
	{
		float const np = camera.NearPlane();
		float const fp = camera.FarPlane();

		float esm_scale_factor = factor / (fp - np);
		pp_chain_[0]->SetParam(0, esm_scale_factor);
		pp_chain_[1]->SetParam(0, esm_scale_factor);

		float4 near_q_far = camera.NearQFarParam();
		near_q_far.w() = 1 / fp / esm_scale_factor;
		pp_chain_[0]->SetParam(1, near_q_far);
		near_q_far.z() *= esm_scale_factor;
		pp_chain_[1]->SetParam(1, near_q_far);
	}

	void LogGaussianBlurPostProcess::InputPin(uint32_t index, ShaderResourceViewPtr const& srv)
	{
		pp_chain_[0]->InputPin(index, srv);

		if (0 == index)
		{
			auto const& tex = *srv->TextureResource();
			if (!blur_x_tex_ || (blur_x_tex_->Width(0) != tex.Width(0)) || (blur_x_tex_->Height(0) != tex.Height(0)))
			{
				auto& rf = Context::Instance().RenderFactoryInstance();
				blur_x_tex_ = rf.MakeTexture2D(tex.Width(0), tex.Height(0), 1, 1, tex.Format(), 1, 0, EAH_GPU_Read | EAH_GPU_Write);
				blur_x_srv_ = rf.MakeTextureSrv(blur_x_tex_);
				blur_x_rtv_ = rf.Make2DRtv(blur_x_tex_, 0, 1, 0);
			}
			pp_chain_[0]->OutputPin(0, blur_x_rtv_);
			pp_chain_[1]->InputPin(0, blur_x_srv_);
		}
		else
		{
			pp_chain_[1]->InputPin(index, srv);
		}
	}
}
