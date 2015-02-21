#include "Log.hpp"
#include "RuntimeD3D10.hpp"
#include "EffectTree.hpp"

#include <d3dcompiler.h>
#include <nanovg_d3d10.h>
#include <boost\algorithm\string.hpp>

// -----------------------------------------------------------------------------------------------------

namespace ReShade
{
	namespace Runtimes
	{
		namespace
		{
			class D3D10EffectCompiler : private boost::noncopyable
			{
			public:
				D3D10EffectCompiler(const FX::Tree &ast, bool skipoptimization = false) : mAST(ast), mEffect(nullptr), mFatal(false), mSkipShaderOptimization(skipoptimization), mCurrentInParameterBlock(false), mCurrentInFunctionBlock(false), mCurrentInForInitialization(0), mCurrentGlobalSize(0), mCurrentGlobalStorageSize(0)
				{
				}

				bool Traverse(D3D10Effect *effect, std::string &errors)
				{
					this->mEffect = effect;
					this->mErrors.clear();
					this->mFatal = false;
					this->mCurrentSource.clear();

					// Global constant buffer
					this->mEffect->mConstantBuffers.push_back(nullptr);
					this->mEffect->mConstantStorages.push_back(nullptr);

					for (auto type : this->mAST.Types)
					{
						Visit(type);
					}
					for (auto uniform : this->mAST.Uniforms)
					{
						Visit(uniform);
					}
					for (auto function : this->mAST.Functions)
					{
						Visit(function);
					}
					for (auto technique : this->mAST.Techniques)
					{
						Visit(technique);
					}

					if (this->mCurrentGlobalSize != 0)
					{
						CD3D10_BUFFER_DESC globalsDesc(RoundToMultipleOf16(this->mCurrentGlobalSize), D3D10_BIND_CONSTANT_BUFFER, D3D10_USAGE_DYNAMIC, D3D10_CPU_ACCESS_WRITE);
						D3D10_SUBRESOURCE_DATA globalsInitial;
						globalsInitial.pSysMem = this->mEffect->mConstantStorages[0];
						globalsInitial.SysMemPitch = globalsInitial.SysMemSlicePitch = this->mCurrentGlobalSize;
						this->mEffect->mRuntime->mDevice->CreateBuffer(&globalsDesc, &globalsInitial, &this->mEffect->mConstantBuffers[0]);
					}

					errors += this->mErrors;

					return !this->mFatal;
				}

				static inline UINT RoundToMultipleOf16(UINT size)
				{
					return (size + 15) & ~15;
				}
				static D3D10_STENCIL_OP LiteralToStencilOp(unsigned int value)
				{
					if (value == FX::Nodes::Pass::States::ZERO)
					{
						return D3D10_STENCIL_OP_ZERO;
					}
							
					return static_cast<D3D10_STENCIL_OP>(value);
				}
				static D3D10_BLEND LiteralToBlend(unsigned int value)
				{
					switch (value)
					{
						case FX::Nodes::Pass::States::ZERO:
							return D3D10_BLEND_ZERO;
						case FX::Nodes::Pass::States::ONE:
							return D3D10_BLEND_ONE;
					}

					return static_cast<D3D10_BLEND>(value);
				}
				static DXGI_FORMAT LiteralToFormat(unsigned int value, FX::Effect::Texture::Format &name)
				{
					switch (value)
					{
						case FX::Nodes::Variable::Properties::R8:
							name = FX::Effect::Texture::Format::R8;
							return DXGI_FORMAT_R8_UNORM;
						case FX::Nodes::Variable::Properties::R32F:
							name = FX::Effect::Texture::Format::R32F;
							return DXGI_FORMAT_R32_FLOAT;
						case FX::Nodes::Variable::Properties::RG8:
							name = FX::Effect::Texture::Format::RG8;
							return DXGI_FORMAT_R8G8_UNORM;
						case FX::Nodes::Variable::Properties::RGBA8:
							name = FX::Effect::Texture::Format::RGBA8;
							return DXGI_FORMAT_R8G8B8A8_TYPELESS;
						case FX::Nodes::Variable::Properties::RGBA16:
							name = FX::Effect::Texture::Format::RGBA16;
							return DXGI_FORMAT_R16G16B16A16_UNORM;
						case FX::Nodes::Variable::Properties::RGBA16F:
							name = FX::Effect::Texture::Format::RGBA16F;
							return DXGI_FORMAT_R16G16B16A16_FLOAT;
						case FX::Nodes::Variable::Properties::RGBA32F:
							name = FX::Effect::Texture::Format::RGBA32F;
							return DXGI_FORMAT_R32G32B32A32_FLOAT;
						case FX::Nodes::Variable::Properties::DXT1:
							name = FX::Effect::Texture::Format::DXT1;
							return DXGI_FORMAT_BC1_TYPELESS;
						case FX::Nodes::Variable::Properties::DXT3:
							name = FX::Effect::Texture::Format::DXT3;
							return DXGI_FORMAT_BC2_TYPELESS;
						case FX::Nodes::Variable::Properties::DXT5:
							name = FX::Effect::Texture::Format::DXT5;
							return DXGI_FORMAT_BC3_TYPELESS;
						case FX::Nodes::Variable::Properties::LATC1:
							name = FX::Effect::Texture::Format::LATC1;
							return DXGI_FORMAT_BC4_UNORM;
						case FX::Nodes::Variable::Properties::LATC2:
							name = FX::Effect::Texture::Format::LATC2;
							return DXGI_FORMAT_BC5_UNORM;
						default:
							name = FX::Effect::Texture::Format::Unknown;
							return DXGI_FORMAT_UNKNOWN;
					}
				}
				static DXGI_FORMAT MakeTypelessFormat(DXGI_FORMAT format)
				{
					switch (format)
					{
						case DXGI_FORMAT_R8G8B8A8_UNORM:
						case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
							return DXGI_FORMAT_R8G8B8A8_TYPELESS;
						case DXGI_FORMAT_BC1_UNORM:
						case DXGI_FORMAT_BC1_UNORM_SRGB:
							return DXGI_FORMAT_BC1_TYPELESS;
						case DXGI_FORMAT_BC2_UNORM:
						case DXGI_FORMAT_BC2_UNORM_SRGB:
							return DXGI_FORMAT_BC2_TYPELESS;
						case DXGI_FORMAT_BC3_UNORM:
						case DXGI_FORMAT_BC3_UNORM_SRGB:
							return DXGI_FORMAT_BC3_TYPELESS;
						default:
							return format;
					}
				}
				static DXGI_FORMAT MakeSRGBFormat(DXGI_FORMAT format)
				{
					switch (format)
					{
						case DXGI_FORMAT_R8G8B8A8_TYPELESS:
						case DXGI_FORMAT_R8G8B8A8_UNORM:
							return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
						case DXGI_FORMAT_BC1_TYPELESS:
						case DXGI_FORMAT_BC1_UNORM:
							return DXGI_FORMAT_BC1_UNORM_SRGB;
						case DXGI_FORMAT_BC2_TYPELESS:
						case DXGI_FORMAT_BC2_UNORM:
							return DXGI_FORMAT_BC2_UNORM_SRGB;
						case DXGI_FORMAT_BC3_TYPELESS:
						case DXGI_FORMAT_BC3_UNORM:
							return DXGI_FORMAT_BC3_UNORM_SRGB;
						default:
							return format;
					}
				}
				static DXGI_FORMAT MakeNonSRBFormat(DXGI_FORMAT format)
				{
					switch (format)
					{
						case DXGI_FORMAT_R8G8B8A8_TYPELESS:
						case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
							return DXGI_FORMAT_R8G8B8A8_UNORM;
						case DXGI_FORMAT_BC1_TYPELESS:
						case DXGI_FORMAT_BC1_UNORM_SRGB:
							return DXGI_FORMAT_BC1_UNORM;
						case DXGI_FORMAT_BC2_TYPELESS:
						case DXGI_FORMAT_BC2_UNORM_SRGB:
							return DXGI_FORMAT_BC2_UNORM;
						case DXGI_FORMAT_BC3_TYPELESS:
						case DXGI_FORMAT_BC3_UNORM_SRGB:
							return DXGI_FORMAT_BC3_UNORM;
						default:
							return format;
					}
				}
				static std::size_t D3D10_SAMPLER_DESC_HASH(const D3D10_SAMPLER_DESC &s) 
				{
					const unsigned char *p = reinterpret_cast<const unsigned char *>(&s);
					std::size_t h = 2166136261;

					for (std::size_t i = 0; i < sizeof(D3D10_SAMPLER_DESC); ++i)
					{
						h = (h * 16777619) ^ p[i];
					}

					return h;
				}

				static std::string ConvertSemantic(const std::string &semantic)
				{
					if (semantic == "VERTEXID")
					{
						return "SV_VERTEXID";
					}
					else if (semantic == "POSITION" || semantic == "VPOS")
					{
						return "SV_POSITION";
					}
					else if (boost::starts_with(semantic, "COLOR"))
					{
						return "SV_TARGET" + semantic.substr(5);
					}
					else if (semantic == "DEPTH")
					{
						return "SV_DEPTH";
					}

					return semantic;
				}
				static inline std::string PrintLocation(const FX::Lexer::Location &location)
				{
					return location.Source + "(" + std::to_string(location.Line) + ", " + std::to_string(location.Column) + "): ";
				}
				std::string PrintType(const FX::Nodes::Type &type)
				{
					std::string res;

					switch (type.BaseClass)
					{
						case FX::Nodes::Type::Class::Void:
							res += "void";
							break;
						case FX::Nodes::Type::Class::Bool:
							res += "bool";
							break;
						case FX::Nodes::Type::Class::Int:
							res += "int";
							break;
						case FX::Nodes::Type::Class::Uint:
							res += "uint";
							break;
						case FX::Nodes::Type::Class::Float:
							res += "float";
							break;
						case FX::Nodes::Type::Class::Sampler2D:
							res += "__sampler2D";
							break;
						case FX::Nodes::Type::Class::Struct:
							res += type.Definition->Name;
							break;
					}

					if (type.IsMatrix())
					{
						res += std::to_string(type.Rows) + "x" + std::to_string(type.Cols);
					}
					else if (type.IsVector())
					{
						res += std::to_string(type.Rows);
					}

					return res;
				}
				std::string PrintTypeWithQualifiers(const FX::Nodes::Type &type)
				{
					std::string qualifiers;

					if (type.HasQualifier(FX::Nodes::Type::Qualifier::Extern))
						qualifiers += "extern ";
					if (type.HasQualifier(FX::Nodes::Type::Qualifier::Static))
						qualifiers += "static ";
					if (type.HasQualifier(FX::Nodes::Type::Qualifier::Const))
						qualifiers += "const ";
					if (type.HasQualifier(FX::Nodes::Type::Qualifier::Volatile))
						qualifiers += "volatile ";
					if (type.HasQualifier(FX::Nodes::Type::Qualifier::Precise))
						qualifiers += "precise ";
					if (type.HasQualifier(FX::Nodes::Type::Qualifier::Linear))
						qualifiers += "linear ";
					if (type.HasQualifier(FX::Nodes::Type::Qualifier::NoPerspective))
						qualifiers += "noperspective ";
					if (type.HasQualifier(FX::Nodes::Type::Qualifier::Centroid))
						qualifiers += "centroid ";
					if (type.HasQualifier(FX::Nodes::Type::Qualifier::NoInterpolation))
						qualifiers += "nointerpolation ";
					if (type.HasQualifier(FX::Nodes::Type::Qualifier::InOut))
						qualifiers += "inout ";
					else if (type.HasQualifier(FX::Nodes::Type::Qualifier::In))
						qualifiers += "in ";
					else if (type.HasQualifier(FX::Nodes::Type::Qualifier::Out))
						qualifiers += "out ";
					else if (type.HasQualifier(FX::Nodes::Type::Qualifier::Uniform))
						qualifiers += "uniform ";

					return qualifiers + PrintType(type);
				}

				void Visit(const FX::Nodes::Statement *node)
				{
					if (node == nullptr)
					{
						return;
					}

					switch (node->NodeId)
					{
						case FX::Node::Id::Compound:
							Visit(static_cast<const FX::Nodes::Compound *>(node));
							break;
						case FX::Node::Id::DeclaratorList:
							Visit(static_cast<const FX::Nodes::DeclaratorList *>(node));
							break;
						case FX::Node::Id::ExpressionStatement:
							Visit(static_cast<const FX::Nodes::ExpressionStatement *>(node));
							break;
						case FX::Node::Id::If:
							Visit(static_cast<const FX::Nodes::If *>(node));
							break;
						case FX::Node::Id::Switch:
							Visit(static_cast<const FX::Nodes::Switch *>(node));
							break;
						case FX::Node::Id::For:
							Visit(static_cast<const FX::Nodes::For *>(node));
							break;
						case FX::Node::Id::While:
							Visit(static_cast<const FX::Nodes::While *>(node));
							break;
						case FX::Node::Id::Return:
							Visit(static_cast<const FX::Nodes::Return *>(node));
							break;
						case FX::Node::Id::Jump:
							Visit(static_cast<const FX::Nodes::Jump *>(node));
							break;
						default:
							assert(false);
							break;
					}
				}
				void Visit(const FX::Nodes::Expression *node)
				{
					switch (node->NodeId)
					{
						case FX::Node::Id::LValue:
							Visit(static_cast<const FX::Nodes::LValue *>(node));
							break;
						case FX::Node::Id::Literal:
							Visit(static_cast<const FX::Nodes::Literal *>(node));
							break;
						case FX::Node::Id::Sequence:
							Visit(static_cast<const FX::Nodes::Sequence *>(node));
							break;
						case FX::Node::Id::Unary:
							Visit(static_cast<const FX::Nodes::Unary *>(node));
							break;
						case FX::Node::Id::Binary:
							Visit(static_cast<const FX::Nodes::Binary *>(node));
							break;
						case FX::Node::Id::Intrinsic:
							Visit(static_cast<const FX::Nodes::Intrinsic *>(node));
							break;
						case FX::Node::Id::Conditional:
							Visit(static_cast<const FX::Nodes::Conditional *>(node));
							break;
						case FX::Node::Id::Swizzle:
							Visit(static_cast<const FX::Nodes::Swizzle *>(node));
							break;
						case FX::Node::Id::FieldSelection:
							Visit(static_cast<const FX::Nodes::FieldSelection *>(node));
							break;
						case FX::Node::Id::Assignment:
							Visit(static_cast<const FX::Nodes::Assignment *>(node));
							break;
						case FX::Node::Id::Call:
							Visit(static_cast<const FX::Nodes::Call *>(node));
							break;
						case FX::Node::Id::Constructor:
							Visit(static_cast<const FX::Nodes::Constructor *>(node));
							break;
						case FX::Node::Id::InitializerList:
							Visit(static_cast<const FX::Nodes::InitializerList *>(node));
							break;
						default:
							assert(false);
							break;
					}
				}

				void Visit(const FX::Nodes::Compound *node)
				{
					this->mCurrentSource += "{\n";

					for (auto statement : node->Statements)
					{
						Visit(statement);
					}

					this->mCurrentSource += "}\n";
				}
				void Visit(const FX::Nodes::DeclaratorList *node)
				{
					for (auto declarator : node->Declarators)
					{
						Visit(declarator);

						if (this->mCurrentInForInitialization)
						{
							this->mCurrentSource += ", ";
							this->mCurrentInForInitialization++;
						}
						else
						{
							this->mCurrentSource += ";\n";
						}
					}
				}
				void Visit(const FX::Nodes::ExpressionStatement *node)
				{
					Visit(node->Expression);

					this->mCurrentSource += ";\n";
				}
				void Visit(const FX::Nodes::If *node)
				{
					for (auto &attribute : node->Attributes)
					{
						this->mCurrentSource += '[';
						this->mCurrentSource += attribute;
						this->mCurrentSource += ']';
					}

					this->mCurrentSource += "if (";
					Visit(node->Condition);
					this->mCurrentSource += ")\n";

					if (node->StatementOnTrue != nullptr)
					{
						Visit(node->StatementOnTrue);
					}
					else
					{
						this->mCurrentSource += "\t;";
					}
					
					if (node->StatementOnFalse != nullptr)
					{
						this->mCurrentSource += "else\n";
						
						Visit(node->StatementOnFalse);
					}
				}
				void Visit(const FX::Nodes::Switch *node)
				{
					for (auto &attribute : node->Attributes)
					{
						this->mCurrentSource += '[';
						this->mCurrentSource += attribute;
						this->mCurrentSource += ']';
					}

					this->mCurrentSource += "switch (";
					Visit(node->Test);
					this->mCurrentSource += ")\n{\n";

					for (auto cases : node->Cases)
					{
						Visit(cases);
					}

					this->mCurrentSource += "}\n";
				}
				void Visit(const FX::Nodes::Case *node)
				{
					for (auto label : node->Labels)
					{
						if (label == nullptr)
						{
							this->mCurrentSource += "default";
						}
						else
						{
							this->mCurrentSource += "case ";

							Visit(label);
						}

						this->mCurrentSource += ":\n";
					}

					Visit(node->Statements);
				}
				void Visit(const FX::Nodes::For *node)
				{
					for (auto &attribute : node->Attributes)
					{
						this->mCurrentSource += '[';
						this->mCurrentSource += attribute;
						this->mCurrentSource += ']';
					}

					this->mCurrentSource += "for (";

					if (node->Initialization != nullptr)
					{
						this->mCurrentInForInitialization = 1;

						Visit(node->Initialization);

						this->mCurrentInForInitialization = 0;

						this->mCurrentSource.pop_back();
						this->mCurrentSource.pop_back();
					}

					this->mCurrentSource += "; ";
										
					if (node->Condition != nullptr)
					{
						Visit(node->Condition);
					}

					this->mCurrentSource += "; ";

					if (node->Increment != nullptr)
					{
						Visit(node->Increment);
					}

					this->mCurrentSource += ")\n";

					if (node->Statements != nullptr)
					{
						Visit(node->Statements);
					}
					else
					{
						this->mCurrentSource += "\t;";
					}
				}
				void Visit(const FX::Nodes::While *node)
				{
					for (auto &attribute : node->Attributes)
					{
						this->mCurrentSource += '[';
						this->mCurrentSource += attribute;
						this->mCurrentSource += ']';
					}

					if (node->DoWhile)
					{
						this->mCurrentSource += "do\n{\n";

						if (node->Statements != nullptr)
						{
							Visit(node->Statements);
						}

						this->mCurrentSource += "}\n";
						this->mCurrentSource += "while (";
						Visit(node->Condition);
						this->mCurrentSource += ");\n";
					}
					else
					{
						this->mCurrentSource += "while (";
						Visit(node->Condition);
						this->mCurrentSource += ")\n";

						if (node->Statements != nullptr)
						{
							Visit(node->Statements);
						}
						else
						{
							this->mCurrentSource += "\t;";
						}
					}
				}
				void Visit(const FX::Nodes::Return *node)
				{
					if (node->Discard)
					{
						this->mCurrentSource += "discard";
					}
					else
					{
						this->mCurrentSource += "return";

						if (node->Value != nullptr)
						{
							this->mCurrentSource += ' ';

							Visit(node->Value);
						}
					}

					this->mCurrentSource += ";\n";
				}
				void Visit(const FX::Nodes::Jump *node)
				{
					switch (node->Mode)
					{
						case FX::Nodes::Jump::Break:
							this->mCurrentSource += "break";
							break;
						case FX::Nodes::Jump::Continue:
							this->mCurrentSource += "continue";
							break;
					}

					this->mCurrentSource += ";\n";
				}

				void Visit(const FX::Nodes::LValue *node)
				{
					this->mCurrentSource += node->Reference->Name;
				}
				void Visit(const FX::Nodes::Literal *node)
				{
					if (!node->Type.IsScalar())
					{
						this->mCurrentSource += PrintType(node->Type);
						this->mCurrentSource += '(';
					}

					for (unsigned int i = 0; i < node->Type.Rows * node->Type.Cols; ++i)
					{
						switch (node->Type.BaseClass)
						{
							case FX::Nodes::Type::Class::Bool:
								this->mCurrentSource += node->Value.Int[i] ? "true" : "false";
								break;
							case FX::Nodes::Type::Class::Int:
								this->mCurrentSource += std::to_string(node->Value.Int[i]);
								break;
							case FX::Nodes::Type::Class::Uint:
								this->mCurrentSource += std::to_string(node->Value.Uint[i]);
								break;
							case FX::Nodes::Type::Class::Float:
								this->mCurrentSource += std::to_string(node->Value.Float[i]) + "f";
								break;
						}

						this->mCurrentSource += ", ";
					}

					this->mCurrentSource.pop_back();
					this->mCurrentSource.pop_back();

					if (!node->Type.IsScalar())
					{
						this->mCurrentSource += ')';
					}
				}
				void Visit(const FX::Nodes::Sequence *node)
				{
					for (auto expression : node->Expressions)
					{
						Visit(expression);

						this->mCurrentSource += ", ";
					}
					
					this->mCurrentSource.pop_back();
					this->mCurrentSource.pop_back();
				}
				void Visit(const FX::Nodes::Unary *node)
				{
					std::string part1, part2;

					switch (node->Operator)
					{
						case FX::Nodes::Unary::Op::Negate:
							part1 = '-';
							break;
						case FX::Nodes::Unary::Op::BitwiseNot:
							part1 = "~";
							break;
						case FX::Nodes::Unary::Op::LogicalNot:
							part1 = '!';
							break;
						case FX::Nodes::Unary::Op::Increase:
							part1 = "++";
							break;
						case FX::Nodes::Unary::Op::Decrease:
							part1 = "--";
							break;
						case FX::Nodes::Unary::Op::PostIncrease:
							part2 = "++";
							break;
						case FX::Nodes::Unary::Op::PostDecrease:
							part2 = "--";
							break;
						case FX::Nodes::Unary::Op::Cast:
							part1 = PrintType(node->Type) + '(';
							part2 = ')';
							break;
					}

					this->mCurrentSource += part1;
					Visit(node->Operand);
					this->mCurrentSource += part2;
				}
				void Visit(const FX::Nodes::Binary *node)
				{
					std::string part1, part2, part3;

					switch (node->Operator)
					{
						case FX::Nodes::Binary::Op::Add:
							part1 = '(';
							part2 = " + ";
							part3 = ')';
							break;
						case FX::Nodes::Binary::Op::Subtract:
							part1 = '(';
							part2 = " - ";
							part3 = ')';
							break;
						case FX::Nodes::Binary::Op::Multiply:
							part1 = '(';
							part2 = " * ";
							part3 = ')';
							break;
						case FX::Nodes::Binary::Op::Divide:
							part1 = '(';
							part2 = " / ";
							part3 = ')';
							break;
						case FX::Nodes::Binary::Op::Modulo:
							part1 = '(';
							part2 = " % ";
							part3 = ')';
							break;
						case FX::Nodes::Binary::Op::Less:
							part1 = '(';
							part2 = " < ";
							part3 = ')';
							break;
						case FX::Nodes::Binary::Op::Greater:
							part1 = '(';
							part2 = " > ";
							part3 = ')';
							break;
						case FX::Nodes::Binary::Op::LessOrEqual:
							part1 = '(';
							part2 = " <= ";
							part3 = ')';
							break;
						case FX::Nodes::Binary::Op::GreaterOrEqual:
							part1 = '(';
							part2 = " >= ";
							part3 = ')';
							break;
						case FX::Nodes::Binary::Op::Equal:
							part1 = '(';
							part2 = " == ";
							part3 = ')';
							break;
						case FX::Nodes::Binary::Op::NotEqual:
							part1 = '(';
							part2 = " != ";
							part3 = ')';
							break;
						case FX::Nodes::Binary::Op::LeftShift:
							part1 = "(";
							part2 = " << ";
							part3 = ")";
							break;
						case FX::Nodes::Binary::Op::RightShift:
							part1 = "(";
							part2 = " >> ";
							part3 = ")";
							break;
						case FX::Nodes::Binary::Op::BitwiseAnd:
							part1 = "(";
							part2 = " & ";
							part3 = ")";
							break;
						case FX::Nodes::Binary::Op::BitwiseOr:
							part1 = "(";
							part2 = " | ";
							part3 = ")";
							break;
						case FX::Nodes::Binary::Op::BitwiseXor:
							part1 = "(";
							part2 = " ^ ";
							part3 = ")";
							break;
						case FX::Nodes::Binary::Op::LogicalAnd:
							part1 = '(';
							part2 = " && ";
							part3 = ')';
							break;
						case FX::Nodes::Binary::Op::LogicalOr:
							part1 = '(';
							part2 = " || ";
							part3 = ')';
							break;
						case FX::Nodes::Binary::Op::ElementExtract:
							part2 = '[';
							part3 = ']';
							break;
					}

					this->mCurrentSource += part1;
					Visit(node->Operands[0]);
					this->mCurrentSource += part2;
					Visit(node->Operands[1]);
					this->mCurrentSource += part3;
				}
				void Visit(const FX::Nodes::Intrinsic *node)
				{
					std::string part1, part2, part3, part4;

					switch (node->Operator)
					{
						case FX::Nodes::Intrinsic::Op::Abs:
							part1 = "abs(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Acos:
							part1 = "acos(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::All:
							part1 = "all(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Any:
							part1 = "any(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::BitCastInt2Float:
							part1 = "asfloat(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::BitCastUint2Float:
							part1 = "asfloat(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Asin:
							part1 = "asin(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::BitCastFloat2Int:
							part1 = "asint(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::BitCastFloat2Uint:
							part1 = "asuint(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Atan:
							part1 = "atan(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Atan2:
							part1 = "atan2(";
							part2 = ", ";
							part3 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Ceil:
							part1 = "ceil(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Clamp:
							part1 = "clamp(";
							part2 = ", ";
							part3 = ", ";
							part4 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Cos:
							part1 = "cos(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Cosh:
							part1 = "cosh(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Cross:
							part1 = "cross(";
							part2 = ", ";
							part3 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::PartialDerivativeX:
							part1 = "ddx(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::PartialDerivativeY:
							part1 = "ddy(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Degrees:
							part1 = "degrees(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Determinant:
							part1 = "determinant(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Distance:
							part1 = "distance(";
							part2 = ", ";
							part3 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Dot:
							part1 = "dot(";
							part2 = ", ";
							part3 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Exp:
							part1 = "exp(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Exp2:
							part1 = "exp2(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::FaceForward:
							part1 = "faceforward(";
							part2 = ", ";
							part3 = ", ";
							part4 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Floor:
							part1 = "floor(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Frac:
							part1 = "frac(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Frexp:
							part1 = "frexp(";
							part2 = ", ";
							part3 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Fwidth:
							part1 = "fwidth(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Ldexp:
							part1 = "ldexp(";
							part2 = ", ";
							part3 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Length:
							part1 = "length(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Lerp:
							part1 = "lerp(";
							part2 = ", ";
							part3 = ", ";
							part4 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Log:
							part1 = "log(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Log10:
							part1 = "log10(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Log2:
							part1 = "log2(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Mad:
							part1 = "((";
							part2 = ") * (";
							part3 = ") + (";
							part4 = "))";
							break;
						case FX::Nodes::Intrinsic::Op::Max:
							part1 = "max(";
							part2 = ", ";
							part3 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Min:
							part1 = "min(";
							part2 = ", ";
							part3 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Modf:
							part1 = "modf(";
							part2 = ", ";
							part3 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Mul:
							part1 = "mul(";
							part2 = ", ";
							part3 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Normalize:
							part1 = "normalize(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Pow:
							part1 = "pow(";
							part2 = ", ";
							part3 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Radians:
							part1 = "radians(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Rcp:
							part1 = "(1.0f / ";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Reflect:
							part1 = "reflect(";
							part2 = ", ";
							part3 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Refract:
							part1 = "refract(";
							part2 = ", ";
							part3 = ", ";
							part4 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Round:
							part1 = "round(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Rsqrt:
							part1 = "rsqrt(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Saturate:
							part1 = "saturate(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Sign:
							part1 = "sign(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Sin:
							part1 = "sin(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::SinCos:
							part1 = "sincos(";
							part2 = ", ";
							part3 = ", ";
							part4 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Sinh:
							part1 = "sinh(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::SmoothStep:
							part1 = "smoothstep(";
							part2 = ", ";
							part3 = ", ";
							part4 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Sqrt:
							part1 = "sqrt(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Step:
							part1 = "step(";
							part2 = ", ";
							part3 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Tan:
							part1 = "tan(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Tanh:
							part1 = "tanh(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Tex2D:
							part1 = "__tex2D(";
							part2 = ", ";
							part3 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Tex2DFetch:
							part1 = "__tex2Dfetch(";
							part2 = ", ";
							part3 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Tex2DGather:
							part1 = "__tex2Dgather(";
							part2 = ", ";
							part3 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Tex2DGatherOffset:
							part1 = "__tex2Dgatheroffset(";
							part2 = ", ";
							part3 = ", ";
							part4 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Tex2DLevel:
							part1 = "__tex2Dlod(";
							part2 = ", ";
							part3 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Tex2DLevelOffset:
							part1 = "__tex2Dlodoffset(";
							part2 = ", ";
							part3 = ", ";
							part4 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Tex2DOffset:
							part1 = "__tex2Doffset(";
							part2 = ", ";
							part3 = ", ";
							part4 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Tex2DSize:
							part1 = "__tex2Dsize(";
							part2 = ", ";
							part3 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Transpose:
							part1 = "transpose(";
							part2 = ")";
							break;
						case FX::Nodes::Intrinsic::Op::Trunc:
							part1 = "trunc(";
							part2 = ")";
							break;
					}

					this->mCurrentSource += part1;

					if (node->Arguments[0] != nullptr)
					{
						Visit(node->Arguments[0]);
					}

					this->mCurrentSource += part2;

					if (node->Arguments[1] != nullptr)
					{
						Visit(node->Arguments[1]);
					}

					this->mCurrentSource += part3;

					if (node->Arguments[2] != nullptr)
					{
						Visit(node->Arguments[2]);
					}

					this->mCurrentSource += part4;
				}
				void Visit(const FX::Nodes::Conditional *node)
				{
					this->mCurrentSource += '(';
					Visit(node->Condition);
					this->mCurrentSource += " ? ";
					Visit(node->ExpressionOnTrue);
					this->mCurrentSource += " : ";
					Visit(node->ExpressionOnFalse);
					this->mCurrentSource += ')';
				}
				void Visit(const FX::Nodes::Swizzle *node)
				{
					Visit(node->Operand);

					this->mCurrentSource += '.';

					if (node->Operand->Type.IsMatrix())
					{
						const char swizzle[16][5] =
						{
							"_m00", "_m01", "_m02", "_m03",
							"_m10", "_m11", "_m12", "_m13",
							"_m20", "_m21", "_m22", "_m23",
							"_m30", "_m31", "_m32", "_m33"
						};

						for (int i = 0; i < 4 && node->Mask[i] >= 0; ++i)
						{
							this->mCurrentSource += swizzle[node->Mask[i]];
						}
					}
					else
					{
						const char swizzle[4] =
						{
							'x', 'y', 'z', 'w'
						};

						for (int i = 0; i < 4 && node->Mask[i] >= 0; ++i)
						{
							this->mCurrentSource += swizzle[node->Mask[i]];
						}
					}
				}
				void Visit(const FX::Nodes::FieldSelection *node)
				{
					this->mCurrentSource += '(';

					Visit(node->Operand);

					if (node->Field->Type.HasQualifier(FX::Nodes::Type::Uniform))
					{
						this->mCurrentSource += '_';
					}
					else
					{
						this->mCurrentSource += '.';
					}

					this->mCurrentSource += node->Field->Name;

					this->mCurrentSource += ')';
				}
				void Visit(const FX::Nodes::Assignment *node)
				{
					this->mCurrentSource += '(';
					Visit(node->Left);
					this->mCurrentSource += ' ';

					switch (node->Operator)
					{
						case FX::Nodes::Assignment::Op::None:
							this->mCurrentSource += '=';
							break;
						case FX::Nodes::Assignment::Op::Add:
							this->mCurrentSource += "+=";
							break;
						case FX::Nodes::Assignment::Op::Subtract:
							this->mCurrentSource += "-=";
							break;
						case FX::Nodes::Assignment::Op::Multiply:
							this->mCurrentSource += "*=";
							break;
						case FX::Nodes::Assignment::Op::Divide:
							this->mCurrentSource += "/=";
							break;
						case FX::Nodes::Assignment::Op::Modulo:
							this->mCurrentSource += "%=";
							break;
						case FX::Nodes::Assignment::Op::LeftShift:
							this->mCurrentSource += "<<=";
							break;
						case FX::Nodes::Assignment::Op::RightShift:
							this->mCurrentSource += ">>=";
							break;
						case FX::Nodes::Assignment::Op::BitwiseAnd:
							this->mCurrentSource += "&=";
							break;
						case FX::Nodes::Assignment::Op::BitwiseOr:
							this->mCurrentSource += "|=";
							break;
						case FX::Nodes::Assignment::Op::BitwiseXor:
							this->mCurrentSource += "^=";
							break;
					}

					this->mCurrentSource += ' ';
					Visit(node->Right);
					this->mCurrentSource += ')';
				}
				void Visit(const FX::Nodes::Call *node)
				{
					this->mCurrentSource += node->CalleeName;
					this->mCurrentSource += '(';

					for (auto argument : node->Arguments)
					{
						Visit(argument);

						this->mCurrentSource += ", ";
					}

					if (!node->Arguments.empty())
					{
						this->mCurrentSource.pop_back();
						this->mCurrentSource.pop_back();
					}

					this->mCurrentSource += ')';
				}
				void Visit(const FX::Nodes::Constructor *node)
				{
					this->mCurrentSource += PrintType(node->Type);
					this->mCurrentSource += '(';

					for (auto argument : node->Arguments)
					{
						Visit(argument);

						this->mCurrentSource += ", ";
					}

					if (!node->Arguments.empty())
					{
						this->mCurrentSource.pop_back();
						this->mCurrentSource.pop_back();
					}

					this->mCurrentSource += ')';
				}
				void Visit(const FX::Nodes::InitializerList *node)
				{
					this->mCurrentSource += "{ ";

					for (auto expression : node->Values)
					{
						Visit(expression);

						this->mCurrentSource += ", ";
					}

					this->mCurrentSource += " }";
				}

				template <typename T>
				void Visit(const std::vector<FX::Nodes::Annotation> &annotations, T &object)
				{
					for (auto &annotation : annotations)
					{
						FX::Effect::Annotation data;

						switch (annotation.Value->Type.BaseClass)
						{
							case FX::Nodes::Type::Class::Bool:
							case FX::Nodes::Type::Class::Int:
								data = annotation.Value->Value.Int;
								break;
							case FX::Nodes::Type::Class::Uint:
								data = annotation.Value->Value.Uint;
								break;
							case FX::Nodes::Type::Class::Float:
								data = annotation.Value->Value.Float;
								break;
							case FX::Nodes::Type::Class::String:
								data = annotation.Value->StringValue;
								break;
						}

						object.AddAnnotation(annotation.Name, data);
					}
				}
				void Visit(const FX::Nodes::Struct *node)
				{
					this->mCurrentSource += "struct ";
					this->mCurrentSource += node->Name;
					this->mCurrentSource += "\n{\n";

					if (!node->Fields.empty())
					{
						for (auto field : node->Fields)
						{
							Visit(field);
						}
					}
					else
					{
						this->mCurrentSource += "float _dummy;\n";
					}

					this->mCurrentSource += "};\n";
				}
				void Visit(const FX::Nodes::Variable *node)
				{
					if (!(this->mCurrentInParameterBlock || this->mCurrentInFunctionBlock))
					{
						if (node->Type.IsStruct() && node->Type.HasQualifier(FX::Nodes::Type::Qualifier::Uniform))
						{
							VisitUniformBuffer(node);
							return;
						}
						else if (node->Type.IsTexture())
						{
							VisitTexture(node);
							return;
						}
						else if (node->Type.IsSampler())
						{
							VisitSampler(node);
							return;
						}
						else if (node->Type.HasQualifier(FX::Nodes::Type::Qualifier::Uniform))
						{
							VisitUniform(node);
							return;
						}
					}

					if (this->mCurrentInForInitialization <= 1)
					{
						this->mCurrentSource += PrintTypeWithQualifiers(node->Type);
					}

					if (!node->Name.empty())
					{
						this->mCurrentSource += ' ';

						if (!this->mCurrentBlockName.empty())
						{
							this->mCurrentSource += this->mCurrentBlockName + '_';
						}
				
						this->mCurrentSource += node->Name;
					}

					if (node->Type.IsArray())
					{
						this->mCurrentSource += '[';
						this->mCurrentSource += (node->Type.ArrayLength >= 1) ? std::to_string(node->Type.ArrayLength) : "";
						this->mCurrentSource += ']';
					}

					if (!node->Semantic.empty())
					{
						this->mCurrentSource += " : " + ConvertSemantic(node->Semantic);
					}

					if (node->Initializer != nullptr)
					{
						this->mCurrentSource += " = ";

						Visit(node->Initializer);
					}

					if (!(this->mCurrentInParameterBlock || this->mCurrentInFunctionBlock))
					{
						this->mCurrentSource += ";\n";
					}
				}
				void VisitTexture(const FX::Nodes::Variable *node)
				{
					D3D10_TEXTURE2D_DESC texdesc;
					ZeroMemory(&texdesc, sizeof(D3D10_TEXTURE2D_DESC));
					D3D10Texture::Description objdesc;

					texdesc.Width = objdesc.Width = node->Properties.Width;
					texdesc.Height = objdesc.Height = node->Properties.Height;
					texdesc.MipLevels = objdesc.Levels = node->Properties.MipLevels;
					texdesc.ArraySize = 1;
					texdesc.Format = LiteralToFormat(node->Properties.Format, objdesc.Format);
					texdesc.SampleDesc.Count = 1;
					texdesc.SampleDesc.Quality = 0;
					texdesc.Usage = D3D10_USAGE_DEFAULT;
					texdesc.BindFlags = D3D10_BIND_SHADER_RESOURCE | D3D10_BIND_RENDER_TARGET;
					texdesc.MiscFlags = D3D10_RESOURCE_MISC_GENERATE_MIPS;

					D3D10Texture *obj = new D3D10Texture(this->mEffect, objdesc);
					obj->mRegister = this->mEffect->mShaderResources.size();
					obj->mTexture = nullptr;
					obj->mShaderResourceView[0] = nullptr;
					obj->mShaderResourceView[1] = nullptr;

					Visit(node->Annotations, *obj);

					if (node->Semantic == "COLOR" || node->Semantic == "SV_TARGET")
					{
						obj->mSource = D3D10Texture::Source::BackBuffer;
						obj->ChangeSource(this->mEffect->mRuntime->mBackBufferTextureSRV[0], this->mEffect->mRuntime->mBackBufferTextureSRV[1]);
					}
					else if (node->Semantic == "DEPTH" || node->Semantic == "SV_DEPTH")
					{
						obj->mSource = D3D10Texture::Source::DepthStencil;
						obj->ChangeSource(this->mEffect->mRuntime->mDepthStencilTextureSRV, nullptr);
					}

					if (obj->mSource != D3D10Texture::Source::Memory)
					{
						if (texdesc.Width != 1 || texdesc.Height != 1 || texdesc.MipLevels != 1 || texdesc.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS)
						{
							this->mErrors += PrintLocation(node->Location) + "warning: texture property on backbuffer textures are ignored.\n";
						}
					}
					else
					{
						if (texdesc.MipLevels == 0)
						{
							this->mErrors += PrintLocation(node->Location) + "warning: a texture cannot have 0 miplevels, changed it to 1.\n";

							texdesc.MipLevels = 1;
						}

						HRESULT hr = this->mEffect->mRuntime->mDevice->CreateTexture2D(&texdesc, nullptr, &obj->mTexture);

						if (FAILED(hr))
						{
							this->mErrors += PrintLocation(node->Location) + "error: 'ID3D10Device::CreateTexture2D' failed with " + std::to_string(hr) + "!\n";
							this->mFatal = true;
							return;
						}

						D3D10_SHADER_RESOURCE_VIEW_DESC srvdesc;
						ZeroMemory(&srvdesc, sizeof(D3D10_SHADER_RESOURCE_VIEW_DESC));
						srvdesc.ViewDimension = D3D10_SRV_DIMENSION_TEXTURE2D;
						srvdesc.Texture2D.MipLevels = texdesc.MipLevels;
						srvdesc.Format = MakeNonSRBFormat(texdesc.Format);

						hr = this->mEffect->mRuntime->mDevice->CreateShaderResourceView(obj->mTexture, &srvdesc, &obj->mShaderResourceView[0]);

						if (FAILED(hr))
						{
							this->mErrors += PrintLocation(node->Location) + "error: 'ID3D10Device::CreateShaderResourceView' failed with " + std::to_string(hr) + "!\n";
							this->mFatal = true;
							return;
						}

						srvdesc.Format = MakeSRGBFormat(texdesc.Format);

						if (srvdesc.Format != texdesc.Format)
						{
							hr = this->mEffect->mRuntime->mDevice->CreateShaderResourceView(obj->mTexture, &srvdesc, &obj->mShaderResourceView[1]);

							if (FAILED(hr))
							{
								this->mErrors += PrintLocation(node->Location) + "error: 'ID3D10Device::CreateShaderResourceView' failed with " + std::to_string(hr) + "!\n";
								this->mFatal = true;
								return;
							}
						}
					}

					this->mCurrentSource += "Texture2D ";
					this->mCurrentSource += node->Name;
					this->mCurrentSource += " : register(t" + std::to_string(this->mEffect->mShaderResources.size()) + "), __";
					this->mCurrentSource += node->Name;
					this->mCurrentSource += "SRGB : register(t" + std::to_string(this->mEffect->mShaderResources.size() + 1) + ");\n";

					this->mEffect->mShaderResources.push_back(obj->mShaderResourceView[0]);
					this->mEffect->mShaderResources.push_back(obj->mShaderResourceView[1]);

					this->mEffect->AddTexture(node->Name, obj);
				}
				void VisitSampler(const FX::Nodes::Variable *node)
				{
					if (node->Properties.Texture == nullptr)
					{
						this->mErrors += PrintLocation(node->Location) + "error: sampler '" + node->Name + "' is missing required 'Texture' required.\n";
						this->mFatal = true;
						return;
					}

					D3D10_SAMPLER_DESC desc;
					desc.AddressU = static_cast<D3D10_TEXTURE_ADDRESS_MODE>(node->Properties.AddressU);
					desc.AddressV = static_cast<D3D10_TEXTURE_ADDRESS_MODE>(node->Properties.AddressV);
					desc.AddressW = static_cast<D3D10_TEXTURE_ADDRESS_MODE>(node->Properties.AddressW);
					desc.MipLODBias = node->Properties.MipLODBias;
					desc.MinLOD = node->Properties.MinLOD;
					desc.MaxLOD = node->Properties.MaxLOD;
					desc.MaxAnisotropy = node->Properties.MaxAnisotropy;
					desc.ComparisonFunc = D3D10_COMPARISON_NEVER;

					const UINT minfilter = node->Properties.MinFilter;
					const UINT magfilter = node->Properties.MagFilter;
					const UINT mipfilter = node->Properties.MipFilter;

					if (minfilter == FX::Nodes::Variable::Properties::ANISOTROPIC || magfilter == FX::Nodes::Variable::Properties::ANISOTROPIC || mipfilter == FX::Nodes::Variable::Properties::ANISOTROPIC)
					{
						desc.Filter = D3D10_FILTER_ANISOTROPIC;
					}
					else if (minfilter == FX::Nodes::Variable::Properties::POINT && magfilter == FX::Nodes::Variable::Properties::POINT && mipfilter == FX::Nodes::Variable::Properties::LINEAR)
					{
						desc.Filter = D3D10_FILTER_MIN_MAG_POINT_MIP_LINEAR;
					}
					else if (minfilter == FX::Nodes::Variable::Properties::POINT && magfilter == FX::Nodes::Variable::Properties::LINEAR && mipfilter == FX::Nodes::Variable::Properties::POINT)
					{
						desc.Filter = D3D10_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
					}
					else if (minfilter == FX::Nodes::Variable::Properties::POINT && magfilter == FX::Nodes::Variable::Properties::LINEAR && mipfilter == FX::Nodes::Variable::Properties::LINEAR)
					{
						desc.Filter = D3D10_FILTER_MIN_POINT_MAG_MIP_LINEAR;
					}
					else if (minfilter == FX::Nodes::Variable::Properties::LINEAR && magfilter == FX::Nodes::Variable::Properties::POINT && mipfilter == FX::Nodes::Variable::Properties::POINT)
					{
						desc.Filter = D3D10_FILTER_MIN_LINEAR_MAG_MIP_POINT;
					}
					else if (minfilter == FX::Nodes::Variable::Properties::LINEAR && magfilter == FX::Nodes::Variable::Properties::POINT && mipfilter == FX::Nodes::Variable::Properties::LINEAR)
					{
						desc.Filter = D3D10_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
					}
					else if (minfilter == FX::Nodes::Variable::Properties::LINEAR && magfilter == FX::Nodes::Variable::Properties::LINEAR && mipfilter == FX::Nodes::Variable::Properties::POINT)
					{
						desc.Filter = D3D10_FILTER_MIN_MAG_LINEAR_MIP_POINT;
					}
					else if (minfilter == FX::Nodes::Variable::Properties::LINEAR && magfilter == FX::Nodes::Variable::Properties::LINEAR && mipfilter == FX::Nodes::Variable::Properties::LINEAR)
					{
						desc.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
					}
					else
					{
						desc.Filter = D3D10_FILTER_MIN_MAG_MIP_POINT;
					}

					D3D10Texture *texture = static_cast<D3D10Texture *>(this->mEffect->GetTexture(node->Properties.Texture->Name));

					if (texture == nullptr)
					{
						this->mErrors += PrintLocation(node->Location) + "error: texture '" + node->Properties.Texture->Name + "' for sampler '" + std::string(node->Name) + "' is missing due to previous error.\n";
						this->mFatal = true;
						return;
					}

					const std::size_t descHash = D3D10_SAMPLER_DESC_HASH(desc);
					auto it = this->mSamplerDescs.find(descHash);

					if (it == this->mSamplerDescs.end())
					{
						ID3D10SamplerState *sampler = nullptr;

						HRESULT hr = this->mEffect->mRuntime->mDevice->CreateSamplerState(&desc, &sampler);

						if (FAILED(hr))
						{
							this->mErrors += PrintLocation(node->Location) + "error: 'ID3D10Device::CreateSamplerState' failed with " + std::to_string(hr) + "!\n";
							this->mFatal = true;
							return;
						}

						this->mEffect->mSamplerStates.push_back(sampler);
						it = this->mSamplerDescs.emplace(descHash, this->mEffect->mSamplerStates.size() - 1).first;

						this->mCurrentSource += "SamplerState __SamplerState" + std::to_string(it->second) + " : register(s" + std::to_string(it->second) + ");\n";
					}

					this->mCurrentSource += "static const __sampler2D ";
					this->mCurrentSource += node->Name;
					this->mCurrentSource += " = { ";

					if (node->Properties.SRGBTexture && texture->mShaderResourceView[1] != nullptr)
					{
						this->mCurrentSource += "__";
						this->mCurrentSource += node->Properties.Texture->Name;
						this->mCurrentSource += "SRGB";
					}
					else
					{
						this->mCurrentSource += node->Properties.Texture->Name;
					}

					this->mCurrentSource += ", __SamplerState" + std::to_string(it->second) + " };\n";
				}
				void VisitUniform(const FX::Nodes::Variable *node)
				{
					this->mCurrentGlobalConstants += PrintTypeWithQualifiers(node->Type);
					this->mCurrentGlobalConstants += ' ';
					this->mCurrentGlobalConstants += node->Name;

					if (node->Type.IsArray())
					{
						this->mCurrentGlobalConstants += '[';
						this->mCurrentGlobalConstants += (node->Type.ArrayLength >= 1) ? std::to_string(node->Type.ArrayLength) : "";
						this->mCurrentGlobalConstants += ']';
					}

					this->mCurrentGlobalConstants += ";\n";

					D3D10Constant::Description objdesc;
					objdesc.Rows = node->Type.Rows;
					objdesc.Columns = node->Type.Cols;
					objdesc.Elements = node->Type.ArrayLength;
					objdesc.Fields = 0;
					objdesc.Size = node->Type.Rows * node->Type.Cols;

					switch (node->Type.BaseClass)
					{
						case FX::Nodes::Type::Class::Bool:
							objdesc.Size *= sizeof(int);
							objdesc.Type = FX::Effect::Constant::Type::Bool;
							break;
						case FX::Nodes::Type::Class::Int:
							objdesc.Size *= sizeof(int);
							objdesc.Type = FX::Effect::Constant::Type::Int;
							break;
						case FX::Nodes::Type::Class::Uint:
							objdesc.Size *= sizeof(unsigned int);
							objdesc.Type = FX::Effect::Constant::Type::Uint;
							break;
						case FX::Nodes::Type::Class::Float:
							objdesc.Size *= sizeof(float);
							objdesc.Type = FX::Effect::Constant::Type::Float;
							break;
					}

					const UINT alignment = 16 - (this->mCurrentGlobalSize % 16);
					this->mCurrentGlobalSize += static_cast<UINT>((objdesc.Size > alignment && (alignment != 16 || objdesc.Size <= 16)) ? objdesc.Size + alignment : objdesc.Size);

					D3D10Constant *obj = new D3D10Constant(this->mEffect, objdesc);
					obj->mBufferIndex = 0;
					obj->mBufferOffset = this->mCurrentGlobalSize - objdesc.Size;

					Visit(node->Annotations, *obj);

					if (this->mCurrentGlobalSize >= this->mCurrentGlobalStorageSize)
					{
						this->mEffect->mConstantStorages[0] = static_cast<unsigned char *>(::realloc(this->mEffect->mConstantStorages[0], this->mCurrentGlobalStorageSize += 128));
					}

					if (node->Initializer != nullptr && node->Initializer->NodeId == FX::Node::Id::Literal)
					{
						CopyMemory(this->mEffect->mConstantStorages[0] + obj->mBufferOffset, &static_cast<const FX::Nodes::Literal *>(node->Initializer)->Value, objdesc.Size);
					}
					else
					{
						ZeroMemory(this->mEffect->mConstantStorages[0] + obj->mBufferOffset, objdesc.Size);
					}

					this->mEffect->AddConstant(node->Name, obj);
				}
				void VisitUniformBuffer(const FX::Nodes::Variable *node)
				{
					this->mCurrentSource += "cbuffer ";
					this->mCurrentSource += node->Name;
					this->mCurrentSource += " : register(b" + std::to_string(this->mEffect->mConstantBuffers.size()) + ")";
					this->mCurrentSource += "\n{\n";

					this->mCurrentBlockName = node->Name;

					ID3D10Buffer *buffer = nullptr;
					unsigned char *storage = nullptr;
					UINT totalsize = 0, currentsize = 0;

					for (auto field : node->Type.Definition->Fields)
					{
						Visit(field);

						D3D10Constant::Description objdesc;
						objdesc.Rows = field->Type.Rows;
						objdesc.Columns = field->Type.Cols;
						objdesc.Elements = field->Type.ArrayLength;
						objdesc.Fields = 0;
						objdesc.Size = field->Type.Rows * field->Type.Cols;

						switch (field->Type.BaseClass)
						{
							case FX::Nodes::Type::Class::Bool:
								objdesc.Size *= sizeof(int);
								objdesc.Type = FX::Effect::Constant::Type::Bool;
								break;
							case FX::Nodes::Type::Class::Int:
								objdesc.Size *= sizeof(int);
								objdesc.Type = FX::Effect::Constant::Type::Int;
								break;
							case FX::Nodes::Type::Class::Uint:
								objdesc.Size *= sizeof(unsigned int);
								objdesc.Type = FX::Effect::Constant::Type::Uint;
								break;
							case FX::Nodes::Type::Class::Float:
								objdesc.Size *= sizeof(float);
								objdesc.Type = FX::Effect::Constant::Type::Float;
								break;
						}

						const UINT alignment = 16 - (totalsize % 16);
						totalsize += static_cast<UINT>((objdesc.Size > alignment && (alignment != 16 || objdesc.Size <= 16)) ? objdesc.Size + alignment : objdesc.Size);

						D3D10Constant *obj = new D3D10Constant(this->mEffect, objdesc);
						obj->mBufferIndex = this->mEffect->mConstantBuffers.size();
						obj->mBufferOffset = totalsize - objdesc.Size;

						if (totalsize >= currentsize)
						{
							storage = static_cast<unsigned char *>(::realloc(storage, currentsize += 128));
						}

						if (field->Initializer != nullptr && field->Initializer->NodeId == FX::Node::Id::Literal)
						{
							CopyMemory(storage + obj->mBufferOffset, &static_cast<const FX::Nodes::Literal *>(field->Initializer)->Value, objdesc.Size);
						}
						else
						{
							ZeroMemory(storage + obj->mBufferOffset, objdesc.Size);
						}

						this->mEffect->AddConstant(node->Name + '.' + field->Name, obj);
					}

					this->mCurrentBlockName.clear();

					this->mCurrentSource += "};\n";

					D3D10Constant::Description objdesc;
					objdesc.Rows = 0;
					objdesc.Columns = 0;
					objdesc.Elements = 0;
					objdesc.Fields = static_cast<unsigned int>(node->Type.Definition->Fields.size());
					objdesc.Size = totalsize;
					objdesc.Type = FX::Effect::Constant::Type::Struct;

					D3D10Constant *obj = new D3D10Constant(this->mEffect, objdesc);
					obj->mBufferIndex = this->mEffect->mConstantBuffers.size();
					obj->mBufferOffset = 0;

					Visit(node->Annotations, *obj);

					this->mEffect->AddConstant(node->Name, obj);

					D3D10_BUFFER_DESC desc;
					desc.ByteWidth = RoundToMultipleOf16(totalsize);
					desc.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
					desc.Usage = D3D10_USAGE_DYNAMIC;
					desc.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
					desc.MiscFlags = 0;

					D3D10_SUBRESOURCE_DATA initial;
					initial.pSysMem = storage;
					initial.SysMemPitch = initial.SysMemSlicePitch = totalsize;

					HRESULT hr = this->mEffect->mRuntime->mDevice->CreateBuffer(&desc, &initial, &buffer);

					if (SUCCEEDED(hr))
					{
						this->mEffect->mConstantBuffers.push_back(buffer);
						this->mEffect->mConstantStorages.push_back(storage);
					}
				}
				void Visit(const FX::Nodes::Function *node)
				{
					this->mCurrentSource += PrintType(node->ReturnType);
					this->mCurrentSource += ' ';
					this->mCurrentSource += node->Name;
					this->mCurrentSource += '(';

					this->mCurrentInParameterBlock = true;

					for (auto parameter : node->Parameters)
					{
						Visit(parameter);

						this->mCurrentSource += ", ";
					}

					this->mCurrentInParameterBlock = false;

					if (!node->Parameters.empty())
					{
						this->mCurrentSource.pop_back();
						this->mCurrentSource.pop_back();
					}

					this->mCurrentSource += ')';
								
					if (!node->ReturnSemantic.empty())
					{
						this->mCurrentSource += " : " + ConvertSemantic(node->ReturnSemantic);
					}

					this->mCurrentSource += '\n';

					this->mCurrentInFunctionBlock = true;

					Visit(node->Definition);

					this->mCurrentInFunctionBlock = false;
				}
				void Visit(const FX::Nodes::Technique *node)
				{
					D3D10Technique::Description objdesc;
					objdesc.Passes = static_cast<unsigned int>(node->Passes.size());

					D3D10Technique *obj = new D3D10Technique(this->mEffect, objdesc);

					Visit(node->Annotations, *obj);

					for (auto pass : node->Passes)
					{
						Visit(pass, obj->mPasses);
					}

					this->mEffect->AddTechnique(node->Name, obj);
				}
				void Visit(const FX::Nodes::Pass *node, std::vector<D3D10Technique::Pass> &passes)
				{
					D3D10Technique::Pass pass;
					pass.VS = nullptr;
					pass.PS = nullptr;
					pass.BS = nullptr;
					pass.DSS = nullptr;
					pass.StencilRef = 0;
					pass.Viewport.TopLeftX = pass.Viewport.TopLeftY = pass.Viewport.Width = pass.Viewport.Height = 0;
					pass.Viewport.MinDepth = 0.0f;
					pass.Viewport.MaxDepth = 1.0f;
					ZeroMemory(pass.RT, sizeof(pass.RT));
					ZeroMemory(pass.RTSRV, sizeof(pass.RTSRV));
					pass.SRV = this->mEffect->mShaderResources;

					if (node->States.VertexShader != nullptr)
					{
						VisitShader(node->States.VertexShader, "vs", pass);
					}
					if (node->States.PixelShader != nullptr)
					{
						VisitShader(node->States.PixelShader, "ps", pass);
					}

					const int targetIndex = node->States.SRGBWriteEnable ? 1 : 0;
					pass.RT[0] = this->mEffect->mRuntime->mBackBufferTargets[targetIndex];
					pass.RTSRV[0] = this->mEffect->mRuntime->mBackBufferTextureSRV[targetIndex];

					for (unsigned int i = 0; i < 8; ++i)
					{
						if (node->States.RenderTargets[i] == nullptr)
						{
							continue;
						}

						D3D10Texture *texture = static_cast<D3D10Texture *>(this->mEffect->GetTexture(node->States.RenderTargets[i]->Name));

						if (texture == nullptr)
						{
							this->mFatal = true;
							return;
						}

						D3D10_TEXTURE2D_DESC desc;
						texture->mTexture->GetDesc(&desc);

						if (pass.Viewport.Width != 0 && pass.Viewport.Height != 0 && (desc.Width != static_cast<unsigned int>(pass.Viewport.Width) || desc.Height != static_cast<unsigned int>(pass.Viewport.Height)))
						{
							this->mErrors += PrintLocation(node->Location) + "error: cannot use multiple rendertargets with different sized textures.\n";
							this->mFatal = true;
							return;
						}
						else
						{
							pass.Viewport.Width = desc.Width;
							pass.Viewport.Height = desc.Height;
						}

						D3D10_RENDER_TARGET_VIEW_DESC rtvdesc;
						ZeroMemory(&rtvdesc, sizeof(D3D10_RENDER_TARGET_VIEW_DESC));
						rtvdesc.Format = node->States.SRGBWriteEnable ? MakeSRGBFormat(desc.Format) : MakeNonSRBFormat(desc.Format);
						rtvdesc.ViewDimension = desc.SampleDesc.Count > 1 ? D3D10_RTV_DIMENSION_TEXTURE2DMS : D3D10_RTV_DIMENSION_TEXTURE2D;

						if (texture->mRenderTargetView[targetIndex] == nullptr)
						{
							HRESULT hr = this->mEffect->mRuntime->mDevice->CreateRenderTargetView(texture->mTexture, &rtvdesc, &texture->mRenderTargetView[targetIndex]);

							if (FAILED(hr))
							{
								this->mErrors += PrintLocation(node->Location) + "warning: 'CreateRenderTargetView' failed!\n";
							}
						}

						pass.RT[i] = texture->mRenderTargetView[targetIndex];
						pass.RTSRV[i] = texture->mShaderResourceView[targetIndex];
					}

					if (pass.Viewport.Width == 0 && pass.Viewport.Height == 0)
					{
						pass.Viewport.Width = this->mEffect->mRuntime->mSwapChainDesc.BufferDesc.Width;
						pass.Viewport.Height = this->mEffect->mRuntime->mSwapChainDesc.BufferDesc.Height;
					}

					D3D10_DEPTH_STENCIL_DESC ddesc;
					ddesc.DepthEnable = node->States.DepthEnable;
					ddesc.DepthWriteMask = node->States.DepthWriteMask ? D3D10_DEPTH_WRITE_MASK_ALL : D3D10_DEPTH_WRITE_MASK_ZERO;
					ddesc.DepthFunc = static_cast<D3D10_COMPARISON_FUNC>(node->States.DepthFunc);
					ddesc.StencilEnable = node->States.StencilEnable;
					ddesc.StencilReadMask = node->States.StencilReadMask;
					ddesc.StencilWriteMask = node->States.StencilWriteMask;
					ddesc.FrontFace.StencilFunc = ddesc.BackFace.StencilFunc = static_cast<D3D10_COMPARISON_FUNC>(node->States.StencilFunc);
					ddesc.FrontFace.StencilPassOp = ddesc.BackFace.StencilPassOp = LiteralToStencilOp(node->States.StencilOpPass);
					ddesc.FrontFace.StencilFailOp = ddesc.BackFace.StencilFailOp = LiteralToStencilOp(node->States.StencilOpFail);
					ddesc.FrontFace.StencilDepthFailOp = ddesc.BackFace.StencilDepthFailOp = LiteralToStencilOp(node->States.StencilOpDepthFail);
					pass.StencilRef = node->States.StencilRef;

					HRESULT hr = this->mEffect->mRuntime->mDevice->CreateDepthStencilState(&ddesc, &pass.DSS);

					if (FAILED(hr))
					{
						this->mErrors += PrintLocation(node->Location) + "warning: 'ID3D10Device::CreateDepthStencilState' failed!\n";
					}

					D3D10_BLEND_DESC bdesc;
					bdesc.AlphaToCoverageEnable = FALSE;
					bdesc.RenderTargetWriteMask[0] = node->States.RenderTargetWriteMask;
					bdesc.BlendEnable[0] = node->States.BlendEnable;
					bdesc.BlendOp = static_cast<D3D10_BLEND_OP>(node->States.BlendOp);
					bdesc.BlendOpAlpha = static_cast<D3D10_BLEND_OP>(node->States.BlendOpAlpha);
					bdesc.SrcBlend = LiteralToBlend(node->States.SrcBlend);
					bdesc.DestBlend = LiteralToBlend(node->States.DestBlend);

					for (UINT i = 1; i < 8; ++i)
					{
						bdesc.RenderTargetWriteMask[i] = bdesc.RenderTargetWriteMask[0];
						bdesc.BlendEnable[i] = bdesc.BlendEnable[0];
					}

					hr = this->mEffect->mRuntime->mDevice->CreateBlendState(&bdesc, &pass.BS);

					if (FAILED(hr))
					{
						this->mErrors += PrintLocation(node->Location) + "warning: 'ID3D10Device::CreateBlendState' failed!\n";
					}

					for (ID3D10ShaderResourceView *&srv : pass.SRV)
					{
						if (srv == nullptr)
						{
							continue;
						}

						ID3D10Resource *res1, *res2;
						srv->GetResource(&res1);
						res1->Release();

						for (ID3D10RenderTargetView *rtv : pass.RT)
						{
							if (rtv == nullptr)
							{
								continue;
							}

							rtv->GetResource(&res2);
							res2->Release();

							if (res1 == res2)
							{
								srv = nullptr;
								break;
							}
						}
					}

					passes.push_back(std::move(pass));
				}
				void VisitShader(const FX::Nodes::Function *node, const std::string &shadertype, D3D10Technique::Pass &pass)
				{
					std::string source =
						"struct __sampler2D { Texture2D t; SamplerState s; };\n"
						"inline float4 __tex2D(__sampler2D s, float2 c) { return s.t.Sample(s.s, c); }\n"
						"inline float4 __tex2Doffset(__sampler2D s, float2 c, int2 offset) { return s.t.Sample(s.s, c, offset); }\n"
						"inline float4 __tex2Dlod(__sampler2D s, float4 c) { return s.t.SampleLevel(s.s, c.xy, c.w); }\n"
						"inline float4 __tex2Dlodoffset(__sampler2D s, float4 c, int2 offset) { return s.t.SampleLevel(s.s, c.xy, c.w, offset); }\n"
						"inline float4 __tex2Dgather(__sampler2D s, float2 c) { return s.t.Gather(s.s, c); }\n"
						"inline float4 __tex2Dgatheroffset(__sampler2D s, float2 c, int2 offset) { return s.t.Gather(s.s, c, offset); }\n"
						"inline float4 __tex2Dfetch(__sampler2D s, int4 c) { return s.t.Load(c.xyw); }\n"
						"inline int2 __tex2Dsize(__sampler2D s, int lod) { uint w, h, l; s.t.GetDimensions(lod, w, h, l); return int2(w, h); }\n";

					if (!this->mCurrentGlobalConstants.empty())
					{
						source += "cbuffer __GLOBAL__ : register(b0)\n{\n" + this->mCurrentGlobalConstants + "};\n";
					}

					source += this->mCurrentSource;

					LOG(TRACE) << "> Compiling shader '" << node->Name << "':\n\n" << source.c_str() << "\n";

					UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
					ID3DBlob *compiled = nullptr, *errors = nullptr;

					if (this->mSkipShaderOptimization)
					{
						flags |= D3DCOMPILE_SKIP_OPTIMIZATION;
					}

					HRESULT hr = D3DCompile(source.c_str(), source.length(), nullptr, nullptr, nullptr, node->Name.c_str(), (shadertype + "_4_0").c_str(), flags, 0, &compiled, &errors);

					if (errors != nullptr)
					{
						this->mErrors += std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize());

						errors->Release();
					}

					if (FAILED(hr))
					{
						this->mFatal = true;
						return;
					}

					if (shadertype == "vs")
					{
						hr = this->mEffect->mRuntime->mDevice->CreateVertexShader(compiled->GetBufferPointer(), compiled->GetBufferSize(), &pass.VS);
					}
					else if (shadertype == "ps")
					{
						hr = this->mEffect->mRuntime->mDevice->CreatePixelShader(compiled->GetBufferPointer(), compiled->GetBufferSize(), &pass.PS);
					}

					compiled->Release();

					if (FAILED(hr))
					{
						this->mErrors += PrintLocation(node->Location) + "error: 'CreateShader' failed!\n";
						this->mFatal = true;
						return;
					}
				}

			private:
				const FX::Tree &mAST;
				D3D10Effect *mEffect;
				std::string mCurrentSource;
				std::string mErrors;
				bool mFatal, mSkipShaderOptimization;
				std::unordered_map<std::size_t, std::size_t> mSamplerDescs;
				std::string mCurrentGlobalConstants;
				UINT mCurrentGlobalSize, mCurrentGlobalStorageSize, mCurrentInForInitialization;
				std::string mCurrentBlockName;
				bool mCurrentInParameterBlock, mCurrentInFunctionBlock;
			};

			template <typename T>
			inline ULONG SAFE_RELEASE(T *&object)
			{
				if (object == nullptr)
				{
					return 0;
				}

				const ULONG ref = object->Release();

				object = nullptr;

				return ref;
			}
		}

		// -----------------------------------------------------------------------------------------------------

		D3D10Runtime::D3D10Runtime(ID3D10Device *device, IDXGISwapChain *swapchain) : mDevice(device), mSwapChain(swapchain), mStateBlock(nullptr), mBackBuffer(nullptr), mBackBufferReplacement(nullptr), mBackBufferTexture(nullptr), mBackBufferTextureSRV(), mBackBufferTargets(), mDepthStencil(nullptr), mDepthStencilReplacement(nullptr), mDepthStencilTexture(nullptr), mDepthStencilTextureSRV(nullptr), mLost(true)
		{
			assert(this->mDevice != nullptr);
			assert(this->mSwapChain != nullptr);

			this->mDevice->AddRef();
			this->mSwapChain->AddRef();

			ZeroMemory(&this->mSwapChainDesc, sizeof(DXGI_SWAP_CHAIN_DESC));

			IDXGIDevice *dxgidevice = nullptr;
			IDXGIAdapter *adapter = nullptr;

			HRESULT hr = this->mDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgidevice));

			assert(SUCCEEDED(hr));

			hr = dxgidevice->GetAdapter(&adapter);

			dxgidevice->Release();

			assert(SUCCEEDED(hr));

			DXGI_ADAPTER_DESC desc;
			hr = adapter->GetDesc(&desc);

			adapter->Release();

			assert(SUCCEEDED(hr));
			
			this->mVendorId = desc.VendorId;
			this->mDeviceId = desc.DeviceId;
			this->mRendererId = 0xD3D10;

			D3D10_STATE_BLOCK_MASK mask;
			D3D10StateBlockMaskEnableAll(&mask);

			hr = D3D10CreateStateBlock(this->mDevice, &mask, &this->mStateBlock);

			assert(SUCCEEDED(hr));
		}
		D3D10Runtime::~D3D10Runtime()
		{
			assert(this->mLost);

			if (this->mStateBlock != nullptr)
			{
				this->mStateBlock->Release();
			}

			this->mDevice->Release();
			this->mSwapChain->Release();
		}

		bool D3D10Runtime::OnCreateInternal(const DXGI_SWAP_CHAIN_DESC &desc)
		{
			this->mSwapChainDesc = desc;

			HRESULT hr = this->mSwapChain->GetBuffer(0, __uuidof(ID3D10Texture2D), reinterpret_cast<void **>(&this->mBackBuffer));

			assert(SUCCEEDED(hr));

			if (!CreateBackBufferReplacement(this->mBackBuffer, desc.SampleDesc))
			{
				LOG(TRACE) << "Failed to create backbuffer replacement!";

				SAFE_RELEASE(this->mBackBuffer);

				return false;
			}

			D3D10_TEXTURE2D_DESC dstdesc;
			ZeroMemory(&dstdesc, sizeof(D3D10_TEXTURE2D_DESC));
			dstdesc.Width = desc.BufferDesc.Width;
			dstdesc.Height = desc.BufferDesc.Height;
			dstdesc.MipLevels = 1;
			dstdesc.ArraySize = 1;
			dstdesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
			dstdesc.SampleDesc.Count = 1;
			dstdesc.SampleDesc.Quality = 0;
			dstdesc.Usage = D3D10_USAGE_DEFAULT;
			dstdesc.BindFlags = D3D10_BIND_DEPTH_STENCIL;

			ID3D10Texture2D *dstexture = nullptr;
			hr = this->mDevice->CreateTexture2D(&dstdesc, nullptr, &dstexture);

			if (SUCCEEDED(hr))
			{
				hr = this->mDevice->CreateDepthStencilView(dstexture, nullptr, &this->mDefaultDepthStencil);

				SAFE_RELEASE(dstexture);
			}
			if (FAILED(hr))
			{
				LOG(TRACE) << "Failed to create default depthstencil! HRESULT is '" << hr << "'.";

				return false;
			}

			this->mNVG = nvgCreateD3D10(this->mDevice, 0);

			this->mLost = false;

			Runtime::OnCreate(desc.BufferDesc.Width, desc.BufferDesc.Height);

			return true;
		}
		void D3D10Runtime::OnDeleteInternal()
		{
			Runtime::OnDelete();

			nvgDeleteD3D10(this->mNVG);

			this->mNVG = nullptr;

			if (this->mStateBlock != nullptr)
			{
				this->mStateBlock->ReleaseAllDeviceObjects();
			}

			SAFE_RELEASE(this->mBackBuffer);
			SAFE_RELEASE(this->mBackBufferReplacement);
			SAFE_RELEASE(this->mBackBufferTexture);
			SAFE_RELEASE(this->mBackBufferTextureSRV[0]);
			SAFE_RELEASE(this->mBackBufferTextureSRV[1]);
			SAFE_RELEASE(this->mBackBufferTargets[0]);
			SAFE_RELEASE(this->mBackBufferTargets[1]);

			SAFE_RELEASE(this->mDepthStencil);
			SAFE_RELEASE(this->mDepthStencilReplacement);
			SAFE_RELEASE(this->mDepthStencilTexture);
			SAFE_RELEASE(this->mDepthStencilTextureSRV);

			SAFE_RELEASE(this->mDefaultDepthStencil);

			ZeroMemory(&this->mSwapChainDesc, sizeof(DXGI_SWAP_CHAIN_DESC));

			this->mLost = true;
		}
		void D3D10Runtime::OnDrawInternal(unsigned int vertices)
		{
			Runtime::OnDraw(vertices);

			ID3D10DepthStencilView *depthstencil = nullptr;
			this->mDevice->OMGetRenderTargets(0, nullptr, &depthstencil);

			if (depthstencil != nullptr)
			{
				depthstencil->Release();

				if (depthstencil == this->mDefaultDepthStencil)
				{
					return;
				}
				else if (depthstencil == this->mDepthStencilReplacement)
				{
					depthstencil = this->mDepthStencil;
				}

				const auto it = this->mDepthSourceTable.find(depthstencil);

				if (it != this->mDepthSourceTable.end())
				{
					it->second.DrawCallCount = static_cast<FLOAT>(this->mLastDrawCalls);
					it->second.DrawVerticesCount += vertices;
				}
			}
		}
		void D3D10Runtime::OnPresentInternal()
		{
			if (this->mLost)
			{
				LOG(TRACE) << "Failed to present! Runtime is in a lost state.";
				return;
			}

			DetectDepthSource();

			// Capture device state
			this->mStateBlock->Capture();

			ID3D10RenderTargetView *stateblockTargets[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT] = { nullptr };
			ID3D10DepthStencilView *stateblockDepthStencil = nullptr;

			this->mDevice->OMGetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, stateblockTargets, &stateblockDepthStencil);

			// Resolve backbuffer
			if (this->mBackBufferReplacement != this->mBackBuffer)
			{
				this->mDevice->ResolveSubresource(this->mBackBuffer, 0, this->mBackBufferReplacement, 0, this->mSwapChainDesc.BufferDesc.Format);
			}

			// Setup real backbuffer
			this->mDevice->OMSetRenderTargets(1, &this->mBackBufferTargets[0], nullptr);

			// Apply post processing
			Runtime::OnPostProcess();

			// Reset rendertarget
			this->mDevice->OMSetRenderTargets(1, &this->mBackBufferTargets[0], this->mDefaultDepthStencil);

			const D3D10_VIEWPORT viewport = { 0, 0, this->mSwapChainDesc.BufferDesc.Width, this->mSwapChainDesc.BufferDesc.Height, 0.0f, 1.0f };
			this->mDevice->RSSetViewports(1, &viewport);

			// Apply presenting
			Runtime::OnPresent();

			if (this->mLost)
			{
				return;
			}

			// Apply previous device state
			this->mStateBlock->Apply();

			this->mDevice->OMSetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, stateblockTargets, stateblockDepthStencil);

			for (UINT i = 0; i < D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
			{
				SAFE_RELEASE(stateblockTargets[i]);
			}

			SAFE_RELEASE(stateblockDepthStencil);
		}
		void D3D10Runtime::OnGetBackBuffer(ID3D10Texture2D *&buffer)
		{
			if (this->mBackBufferReplacement != nullptr)
			{
				buffer = this->mBackBufferReplacement;
			}
		}
		void D3D10Runtime::OnCreateDepthStencilView(ID3D10Resource *resource, ID3D10DepthStencilView *depthstencil)
		{
			assert(resource != nullptr);
			assert(depthstencil != nullptr);

			// Do not track default depthstencil
			if (this->mLost)
			{
				return;
			}

			ID3D10Texture2D *texture = nullptr;
			const HRESULT hr = resource->QueryInterface(__uuidof(ID3D10Texture2D), reinterpret_cast<void **>(&texture));

			if (FAILED(hr))
			{
				return;
			}

			D3D10_TEXTURE2D_DESC desc;
			texture->GetDesc(&desc);

			SAFE_RELEASE(texture);

			// Early depthstencil rejection
			if (desc.Width != this->mSwapChainDesc.BufferDesc.Width || desc.Height != this->mSwapChainDesc.BufferDesc.Height || desc.SampleDesc.Count > 1)
			{
				return;
			}

			LOG(TRACE) << "Adding depthstencil " << depthstencil << " (Width: " << desc.Width << ", Height: " << desc.Height << ", Format: " << desc.Format << ") to list of possible depth candidates ...";

			// Begin tracking new depthstencil
			const DepthSourceInfo info = { desc.Width, desc.Height };
			this->mDepthSourceTable.emplace(depthstencil, info);
		}
		void D3D10Runtime::OnDeleteDepthStencilView(ID3D10DepthStencilView *depthstencil)
		{
			assert(depthstencil != nullptr);

			const auto it = this->mDepthSourceTable.find(depthstencil);
		
			if (it != this->mDepthSourceTable.end())
			{
				LOG(TRACE) << "Removing depthstencil " << depthstencil << " from list of possible depth candidates ...";

				this->mDepthSourceTable.erase(it);
			}
		}
		void D3D10Runtime::OnSetDepthStencilView(ID3D10DepthStencilView *&depthstencil)
		{
			if (this->mDepthStencilReplacement != nullptr && depthstencil == this->mDepthStencil)
			{
				depthstencil = this->mDepthStencilReplacement;
			}
		}
		void D3D10Runtime::OnGetDepthStencilView(ID3D10DepthStencilView *&depthstencil)
		{
			if (this->mDepthStencilReplacement != nullptr && depthstencil == this->mDepthStencilReplacement)
			{
				depthstencil = this->mDepthStencil;
			}
		}
		void D3D10Runtime::OnClearDepthStencilView(ID3D10DepthStencilView *&depthstencil)
		{
			if (this->mDepthStencilReplacement != nullptr && depthstencil == this->mDepthStencil)
			{
				depthstencil = this->mDepthStencilReplacement;
			}
		}
		void D3D10Runtime::OnCopyResource(ID3D10Resource *&dest, ID3D10Resource *&source)
		{
			if (this->mDepthStencilReplacement != nullptr)
			{
				ID3D10Resource *resource = nullptr;
				this->mDepthStencil->GetResource(&resource);

				if (dest == resource)
				{
					dest = this->mDepthStencilTexture;
				}
				if (source == resource)
				{
					source = this->mDepthStencilTexture;
				}

				resource->Release();
			}
		}

		void D3D10Runtime::DetectDepthSource()
		{
			static int cooldown = 0, traffic = 0;

			if (cooldown-- > 0)
			{
				traffic += (sNetworkUpload + sNetworkDownload) > 0;
				return;
			}
			else
			{
				cooldown = 30;

				if (traffic > 10)
				{
					traffic = 0;
					CreateDepthStencilReplacement(nullptr);
					return;
				}
				else
				{
					traffic = 0;
				}
			}

			if (this->mSwapChainDesc.SampleDesc.Count > 1 || this->mDepthSourceTable.empty())
			{
				return;
			}

			DepthSourceInfo bestInfo = { 0 };
			ID3D10DepthStencilView *best = nullptr;

			for (auto &it : this->mDepthSourceTable)
			{
				if (it.second.DrawCallCount == 0)
				{
					continue;
				}
				else if ((it.second.DrawVerticesCount * (1.2f - it.second.DrawCallCount / this->mLastDrawCalls)) >= (bestInfo.DrawVerticesCount * (1.2f - bestInfo.DrawCallCount / this->mLastDrawCalls)))
				{
					best = it.first;
					bestInfo = it.second;
				}

				it.second.DrawCallCount = it.second.DrawVerticesCount = 0;
			}

			if (best != nullptr && this->mDepthStencil != best)
			{
				LOG(TRACE) << "Switched depth source to depthstencil " << best << ".";

				CreateDepthStencilReplacement(best);
			}
		}
		bool D3D10Runtime::CreateBackBufferReplacement(ID3D10Texture2D *backbuffer, const DXGI_SAMPLE_DESC &samples)
		{
			D3D10_TEXTURE2D_DESC texdesc;
			backbuffer->GetDesc(&texdesc);

			HRESULT hr;

			texdesc.SampleDesc = samples;
			texdesc.BindFlags = D3D10_BIND_RENDER_TARGET;

			if (samples.Count > 1)
			{
				hr = this->mDevice->CreateTexture2D(&texdesc, nullptr, &this->mBackBufferReplacement);

				if (FAILED(hr))
				{
					return false;
				}
			}
			else
			{
				this->mBackBufferReplacement = this->mBackBuffer;
				this->mBackBufferReplacement->AddRef();
			}

			texdesc.Format = D3D10EffectCompiler::MakeTypelessFormat(texdesc.Format);
			texdesc.SampleDesc.Count = 1;
			texdesc.SampleDesc.Quality = 0;
			texdesc.BindFlags = D3D10_BIND_SHADER_RESOURCE;

			hr = this->mDevice->CreateTexture2D(&texdesc, nullptr, &this->mBackBufferTexture);

			if (SUCCEEDED(hr))
			{
				D3D10_SHADER_RESOURCE_VIEW_DESC srvdesc;
				ZeroMemory(&srvdesc, sizeof(D3D10_SHADER_RESOURCE_VIEW_DESC));
				srvdesc.Format = D3D10EffectCompiler::MakeNonSRBFormat(texdesc.Format);	
				srvdesc.ViewDimension = D3D10_SRV_DIMENSION_TEXTURE2D;
				srvdesc.Texture2D.MipLevels = texdesc.MipLevels;

				if (SUCCEEDED(hr))
				{
					hr = this->mDevice->CreateShaderResourceView(this->mBackBufferTexture, &srvdesc, &this->mBackBufferTextureSRV[0]);
				}

				srvdesc.Format = D3D10EffectCompiler::MakeSRGBFormat(texdesc.Format);

				if (SUCCEEDED(hr))
				{
					hr = this->mDevice->CreateShaderResourceView(this->mBackBufferTexture, &srvdesc, &this->mBackBufferTextureSRV[1]);
				}
			}

			if (FAILED(hr))
			{
				SAFE_RELEASE(this->mBackBufferReplacement);
				SAFE_RELEASE(this->mBackBufferTexture);
				SAFE_RELEASE(this->mBackBufferTextureSRV[0]);
				SAFE_RELEASE(this->mBackBufferTextureSRV[1]);

				return false;
			}

			D3D10_RENDER_TARGET_VIEW_DESC rtdesc;
			ZeroMemory(&rtdesc, sizeof(D3D10_RENDER_TARGET_VIEW_DESC));
			rtdesc.Format = D3D10EffectCompiler::MakeNonSRBFormat(texdesc.Format);
			rtdesc.ViewDimension = D3D10_RTV_DIMENSION_TEXTURE2D;
		
			hr = this->mDevice->CreateRenderTargetView(this->mBackBuffer, &rtdesc, &this->mBackBufferTargets[0]);

			if (FAILED(hr))
			{
				SAFE_RELEASE(this->mBackBufferReplacement);
				SAFE_RELEASE(this->mBackBufferTexture);
				SAFE_RELEASE(this->mBackBufferTextureSRV[0]);
				SAFE_RELEASE(this->mBackBufferTextureSRV[1]);

				return false;
			}

			rtdesc.Format = D3D10EffectCompiler::MakeSRGBFormat(texdesc.Format);
		
			hr = this->mDevice->CreateRenderTargetView(this->mBackBuffer, &rtdesc, &this->mBackBufferTargets[1]);

			if (FAILED(hr))
			{
				SAFE_RELEASE(this->mBackBufferReplacement);
				SAFE_RELEASE(this->mBackBufferTexture);
				SAFE_RELEASE(this->mBackBufferTextureSRV[0]);
				SAFE_RELEASE(this->mBackBufferTextureSRV[1]);
				SAFE_RELEASE(this->mBackBufferTargets[0]);

				return false;
			}

			return true;
		}
		bool D3D10Runtime::CreateDepthStencilReplacement(ID3D10DepthStencilView *depthstencil)
		{
			SAFE_RELEASE(this->mDepthStencil);
			SAFE_RELEASE(this->mDepthStencilReplacement);
			SAFE_RELEASE(this->mDepthStencilTexture);
			SAFE_RELEASE(this->mDepthStencilTextureSRV);

			if (depthstencil != nullptr)
			{
				this->mDepthStencil = depthstencil;
				this->mDepthStencil->AddRef();
				this->mDepthStencil->GetResource(reinterpret_cast<ID3D10Resource **>(&this->mDepthStencilTexture));

				D3D10_TEXTURE2D_DESC texdesc;
				this->mDepthStencilTexture->GetDesc(&texdesc);

				HRESULT hr = S_OK;

				if ((texdesc.BindFlags & D3D10_BIND_SHADER_RESOURCE) == 0)
				{
					SAFE_RELEASE(this->mDepthStencilTexture);

					switch (texdesc.Format)
					{
						case DXGI_FORMAT_R16_TYPELESS:
						case DXGI_FORMAT_D16_UNORM:
							texdesc.Format = DXGI_FORMAT_R16_TYPELESS;
							break;
						case DXGI_FORMAT_R32_TYPELESS:
						case DXGI_FORMAT_D32_FLOAT:
							texdesc.Format = DXGI_FORMAT_R32_TYPELESS;
							break;
						default:
						case DXGI_FORMAT_R24G8_TYPELESS:
						case DXGI_FORMAT_D24_UNORM_S8_UINT:
							texdesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
							break;
						case DXGI_FORMAT_R32G8X24_TYPELESS:
						case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
							texdesc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
							break;
					}

					texdesc.BindFlags = D3D10_BIND_DEPTH_STENCIL | D3D10_BIND_SHADER_RESOURCE;

					hr = this->mDevice->CreateTexture2D(&texdesc, nullptr, &this->mDepthStencilTexture);

					if (SUCCEEDED(hr))
					{
						D3D10_DEPTH_STENCIL_VIEW_DESC dsvdesc;
						ZeroMemory(&dsvdesc, sizeof(D3D10_DEPTH_STENCIL_VIEW_DESC));
						dsvdesc.ViewDimension = D3D10_DSV_DIMENSION_TEXTURE2D;

						switch (texdesc.Format)
						{
							case DXGI_FORMAT_R16_TYPELESS:
								dsvdesc.Format = DXGI_FORMAT_D16_UNORM;
								break;
							case DXGI_FORMAT_R32_TYPELESS:
								dsvdesc.Format = DXGI_FORMAT_D32_FLOAT;
								break;
							case DXGI_FORMAT_R24G8_TYPELESS:
								dsvdesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
								break;
							case DXGI_FORMAT_R32G8X24_TYPELESS:
								dsvdesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
								break;
						}

						hr = this->mDevice->CreateDepthStencilView(this->mDepthStencilTexture, &dsvdesc, &this->mDepthStencilReplacement);
					}
				}
				else
				{
					this->mDepthStencilReplacement = this->mDepthStencil;
					this->mDepthStencilReplacement->AddRef();
				}

				if (FAILED(hr))
				{
					LOG(TRACE) << "Failed to create depthstencil replacement texture! HRESULT is '" << hr << "'.";

					SAFE_RELEASE(this->mDepthStencil);
					SAFE_RELEASE(this->mDepthStencilTexture);

					return false;
				}

				D3D10_SHADER_RESOURCE_VIEW_DESC srvdesc;
				ZeroMemory(&srvdesc, sizeof(D3D10_SHADER_RESOURCE_VIEW_DESC));
				srvdesc.ViewDimension = D3D10_SRV_DIMENSION_TEXTURE2D;
				srvdesc.Texture2D.MipLevels = 1;

				switch (texdesc.Format)
				{
					case DXGI_FORMAT_R16_TYPELESS:
						srvdesc.Format = DXGI_FORMAT_R16_FLOAT;
						break;
					case DXGI_FORMAT_R32_TYPELESS:
						srvdesc.Format = DXGI_FORMAT_R32_FLOAT;
						break;
					case DXGI_FORMAT_R24G8_TYPELESS:
						srvdesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
						break;
					case DXGI_FORMAT_R32G8X24_TYPELESS:
						srvdesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
						break;
				}

				hr = this->mDevice->CreateShaderResourceView(this->mDepthStencilTexture, &srvdesc, &this->mDepthStencilTextureSRV);

				if (FAILED(hr))
				{
					LOG(TRACE) << "Failed to create depthstencil replacement resource view! HRESULT is '" << hr << "'.";

					SAFE_RELEASE(this->mDepthStencil);
					SAFE_RELEASE(this->mDepthStencilReplacement);
					SAFE_RELEASE(this->mDepthStencilTexture);

					return false;
				}

				if (this->mDepthStencil != this->mDepthStencilReplacement)
				{
					// Update auto depthstencil
					ID3D10RenderTargetView *targets[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT] = { nullptr };
					ID3D10DepthStencilView *depthstencil = nullptr;

					this->mDevice->OMGetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depthstencil);

					if (depthstencil != nullptr)
					{
						if (depthstencil == this->mDepthStencil)
						{
							this->mDevice->OMSetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, this->mDepthStencilReplacement);
						}

						depthstencil->Release();
					}

					for (UINT i = 0; i < D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
					{
						SAFE_RELEASE(targets[i]);
					}
				}
			}

			// Update effect textures
			D3D10Effect *effect = static_cast<D3D10Effect *>(this->mEffect.get());

			if (effect != nullptr)
			{
				for (auto &it : effect->mTextures)
				{
					D3D10Texture *texture = static_cast<D3D10Texture *>(it.second.get());

					if (texture->mSource == D3D10Texture::Source::DepthStencil)
					{
						texture->ChangeSource(this->mDepthStencilTextureSRV, nullptr);
					}
				}
			}

			return true;
		}

		std::unique_ptr<FX::Effect> D3D10Runtime::CompileEffect(const FX::Tree &ast, std::string &errors) const
		{
			std::unique_ptr<D3D10Effect> effect(new D3D10Effect(shared_from_this()));

			D3D10EffectCompiler visitor(ast, this->mSkipShaderOptimization);
		
			if (!visitor.Traverse(effect.get(), errors))
			{
				return nullptr;
			}

			D3D10_RASTERIZER_DESC rsdesc;
			ZeroMemory(&rsdesc, sizeof(D3D10_RASTERIZER_DESC));
			rsdesc.FillMode = D3D10_FILL_SOLID;
			rsdesc.CullMode = D3D10_CULL_NONE;
			rsdesc.DepthClipEnable = TRUE;

			HRESULT hr = this->mDevice->CreateRasterizerState(&rsdesc, &effect->mRasterizerState);

			if (FAILED(hr))
			{
				return nullptr;
			}

			return std::unique_ptr<FX::Effect>(effect.release());
		}
		void D3D10Runtime::CreateScreenshot(unsigned char *buffer, std::size_t size) const
		{
			if (this->mSwapChainDesc.BufferDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && this->mSwapChainDesc.BufferDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && this->mSwapChainDesc.BufferDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && this->mSwapChainDesc.BufferDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
			{
				LOG(WARNING) << "Screenshots are not supported for backbuffer format " << this->mSwapChainDesc.BufferDesc.Format << ".";
				return;
			}

			if (size < static_cast<std::size_t>(this->mSwapChainDesc.BufferDesc.Width * this->mSwapChainDesc.BufferDesc.Height * 4))
			{
				return;
			}

			D3D10_TEXTURE2D_DESC texdesc;
			ZeroMemory(&texdesc, sizeof(D3D10_TEXTURE2D_DESC));
			texdesc.Width = this->mSwapChainDesc.BufferDesc.Width;
			texdesc.Height = this->mSwapChainDesc.BufferDesc.Height;
			texdesc.Format = this->mSwapChainDesc.BufferDesc.Format;
			texdesc.MipLevels = 1;
			texdesc.ArraySize = 1;
			texdesc.SampleDesc.Count = 1;
			texdesc.SampleDesc.Quality = 0;
			texdesc.Usage = D3D10_USAGE_STAGING;
			texdesc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;

			ID3D10Texture2D *textureStaging = nullptr;
			HRESULT hr = this->mDevice->CreateTexture2D(&texdesc, nullptr, &textureStaging);

			if (FAILED(hr))
			{
				LOG(TRACE) << "Failed to create staging texture for screenshot capture! HRESULT is '" << hr << "'.";
				return;
			}

			this->mDevice->CopyResource(textureStaging, this->mBackBuffer);
				
			D3D10_MAPPED_TEXTURE2D mapped;
			hr = textureStaging->Map(0, D3D10_MAP_READ, 0, &mapped);

			if (FAILED(hr))
			{
				LOG(TRACE) << "Failed to map staging texture with screenshot capture! HRESULT is '" << hr << "'.";

				textureStaging->Release();
				return;
			}

			BYTE *pMem = buffer;
			BYTE *pMapped = static_cast<BYTE *>(mapped.pData);

			const UINT pitch = texdesc.Width * 4;

			for (UINT y = 0; y < texdesc.Height; ++y)
			{
				CopyMemory(pMem, pMapped, std::min(pitch, static_cast<UINT>(mapped.RowPitch)));
			
				for (UINT x = 0; x < pitch; x += 4)
				{
					pMem[x + 3] = 0xFF;

					if (this->mSwapChainDesc.BufferDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || this->mSwapChainDesc.BufferDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
					{
						std::swap(pMem[x + 0], pMem[x + 2]);
					}
				}
								
				pMem += pitch;
				pMapped += mapped.RowPitch;
			}

			textureStaging->Unmap(0);

			textureStaging->Release();
		}

		D3D10Effect::D3D10Effect(std::shared_ptr<const D3D10Runtime> runtime) : mRuntime(runtime), mRasterizerState(nullptr), mConstantsDirty(true)
		{
		}
		D3D10Effect::~D3D10Effect()
		{
			SAFE_RELEASE(this->mRasterizerState);

			for (auto &it : this->mSamplerStates)
			{
				it->Release();
			}
			
			for (auto &it : this->mConstantBuffers)
			{
				if (it != nullptr)
				{
					it->Release();
				}
			}
			for (auto &it : this->mConstantStorages)
			{
				free(it);
			}
		}

		void D3D10Effect::Begin() const
		{
			ID3D10Device *const device = this->mRuntime->mDevice;

			// Setup vertex input
			const uintptr_t null = 0;
			device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			device->IASetInputLayout(nullptr);
			device->IASetVertexBuffers(0, 1, reinterpret_cast<ID3D10Buffer *const *>(&null), reinterpret_cast<const UINT *>(&null), reinterpret_cast<const UINT *>(&null));

			device->RSSetState(this->mRasterizerState);

			// Setup samplers
			device->VSSetSamplers(0, static_cast<UINT>(this->mSamplerStates.size()), this->mSamplerStates.data());
			device->PSSetSamplers(0, static_cast<UINT>(this->mSamplerStates.size()), this->mSamplerStates.data());

			// Setup shader constants
			device->VSSetConstantBuffers(0, static_cast<UINT>(this->mConstantBuffers.size()), this->mConstantBuffers.data());
			device->PSSetConstantBuffers(0, static_cast<UINT>(this->mConstantBuffers.size()), this->mConstantBuffers.data());

			// Clear depthstencil
			assert(this->mRuntime->mDefaultDepthStencil != nullptr);

			device->ClearDepthStencilView(this->mRuntime->mDefaultDepthStencil, D3D10_CLEAR_DEPTH | D3D10_CLEAR_STENCIL, 1.0f, 0);
		}
		void D3D10Effect::End() const
		{
		}

		D3D10Texture::D3D10Texture(D3D10Effect *effect, const Description &desc) : Texture(desc), mEffect(effect), mSource(Source::Memory), mTexture(nullptr), mShaderResourceView(), mRenderTargetView(), mRegister(0)
		{
		}
		D3D10Texture::~D3D10Texture()
		{
			SAFE_RELEASE(this->mRenderTargetView[0]);
			SAFE_RELEASE(this->mRenderTargetView[1]);
			SAFE_RELEASE(this->mShaderResourceView[0]);
			SAFE_RELEASE(this->mShaderResourceView[1]);

			SAFE_RELEASE(this->mTexture);
		}

		bool D3D10Texture::Update(unsigned int level, const unsigned char *data, std::size_t size)
		{
			if (data == nullptr || size == 0 || level > this->mDesc.Levels || this->mSource != Source::Memory)
			{
				return false;
			}

			assert(this->mDesc.Height != 0);

			ID3D10Device *device = this->mEffect->mRuntime->mDevice;

			device->UpdateSubresource(this->mTexture, level, nullptr, data, static_cast<UINT>(size / this->mDesc.Height), static_cast<UINT>(size));

			if (level == 0 && this->mDesc.Levels > 1)
			{
				device->GenerateMips(this->mShaderResourceView[0]);
			}

			return true;
		}
		void D3D10Texture::ChangeSource(ID3D10ShaderResourceView *srv, ID3D10ShaderResourceView *srvSRGB)
		{
			if (srvSRGB == nullptr)
			{
				srvSRGB = srv;
			}

			if (srv == this->mShaderResourceView[0] && srvSRGB == this->mShaderResourceView[1])
			{
				return;
			}

			SAFE_RELEASE(this->mRenderTargetView[0]);
			SAFE_RELEASE(this->mRenderTargetView[1]);
			SAFE_RELEASE(this->mShaderResourceView[0]);
			SAFE_RELEASE(this->mShaderResourceView[1]);

			SAFE_RELEASE(this->mTexture);

			if (srv != nullptr)
			{
				this->mShaderResourceView[0] = srv;
				this->mShaderResourceView[0]->AddRef();
				this->mShaderResourceView[0]->GetResource(reinterpret_cast<ID3D10Resource **>(&this->mTexture));
				this->mShaderResourceView[1] = srvSRGB;
				this->mShaderResourceView[1]->AddRef();

				D3D10_TEXTURE2D_DESC texdesc;
				this->mTexture->GetDesc(&texdesc);

				this->mDesc.Width = texdesc.Width;
				this->mDesc.Height = texdesc.Height;
				this->mDesc.Format = FX::Effect::Texture::Format::Unknown;
				this->mDesc.Levels = texdesc.MipLevels;
			}
			else
			{
				this->mDesc.Width = this->mDesc.Height = this->mDesc.Levels = 0;
				this->mDesc.Format = FX::Effect::Texture::Format::Unknown;
			}

			// Update techniques shader resourceviews
			for (auto &technique : this->mEffect->mTechniques)
			{
				for (auto &pass : static_cast<D3D10Technique *>(technique.second.get())->mPasses)
				{
					pass.SRV[this->mRegister] = this->mShaderResourceView[0];
					pass.SRV[this->mRegister + 1] = this->mShaderResourceView[1];
				}
			}
		}

		D3D10Constant::D3D10Constant(D3D10Effect *effect, const Description &desc) : Constant(desc), mEffect(effect), mBufferIndex(0), mBufferOffset(0)
		{
		}
		D3D10Constant::~D3D10Constant()
		{
		}

		void D3D10Constant::GetValue(unsigned char *data, std::size_t size) const
		{
			size = std::min(size, this->mDesc.Size);

			CopyMemory(data, this->mEffect->mConstantStorages[this->mBufferIndex] + this->mBufferOffset, size);
		}
		void D3D10Constant::SetValue(const unsigned char *data, std::size_t size)
		{
			size = std::min(size, this->mDesc.Size);

			unsigned char *storage = this->mEffect->mConstantStorages[this->mBufferIndex] + this->mBufferOffset;

			if (std::memcmp(storage, data, size) == 0)
			{
				return;
			}

			CopyMemory(storage, data, size);

			this->mEffect->mConstantsDirty = true;
		}

		D3D10Technique::D3D10Technique(D3D10Effect *effect, const Description &desc) : Technique(desc), mEffect(effect)
		{
		}
		D3D10Technique::~D3D10Technique()
		{
			for (auto &pass : this->mPasses)
			{
				if (pass.VS != nullptr)
				{
					pass.VS->Release();
				}
				if (pass.PS != nullptr)
				{
					pass.PS->Release();
				}
				if (pass.BS != nullptr)
				{
					pass.BS->Release();
				}
				if (pass.DSS != nullptr)
				{
					pass.DSS->Release();
				}
			}
		}

		void D3D10Technique::RenderPass(unsigned int index) const
		{
			const std::shared_ptr<const D3D10Runtime> runtime = this->mEffect->mRuntime;
			ID3D10Device *const device = runtime->mDevice;
			const D3D10Technique::Pass &pass = this->mPasses[index];

			// Update shader constants
			if (this->mEffect->mConstantsDirty)
			{
				for (std::size_t i = 0, count = this->mEffect->mConstantBuffers.size(); i < count; ++i)
				{
					ID3D10Buffer *buffer = this->mEffect->mConstantBuffers[i];
					const unsigned char *storage = this->mEffect->mConstantStorages[i];

					if (buffer == nullptr)
					{
						continue;
					}

					void *data = nullptr;

					const HRESULT hr = buffer->Map(D3D10_MAP_WRITE_DISCARD, 0, &data);

					if (FAILED(hr))
					{
						LOG(TRACE) << "Failed to map constant buffer at slot " << i << "! HRESULT is '" << hr << "'!";

						continue;
					}

					D3D10_BUFFER_DESC desc;
					buffer->GetDesc(&desc);

					CopyMemory(data, storage, desc.ByteWidth);

					buffer->Unmap();
				}

				this->mEffect->mConstantsDirty = false;
			}

			// Setup states
			device->VSSetShader(pass.VS);
			device->GSSetShader(nullptr);
			device->PSSetShader(pass.PS);

			const FLOAT blendfactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
			device->OMSetBlendState(pass.BS, blendfactor, D3D10_DEFAULT_SAMPLE_MASK);
			device->OMSetDepthStencilState(pass.DSS, pass.StencilRef);

			// Save backbuffer of previous pass
			device->CopyResource(runtime->mBackBufferTexture, runtime->mBackBuffer);

			// Setup shader resources
			device->VSSetShaderResources(0, static_cast<UINT>(pass.SRV.size()), pass.SRV.data());
			device->PSSetShaderResources(0, static_cast<UINT>(pass.SRV.size()), pass.SRV.data());

			// Setup rendertargets
			device->OMSetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, pass.RT, (pass.Viewport.Width == runtime->mSwapChainDesc.BufferDesc.Width && pass.Viewport.Height == runtime->mSwapChainDesc.BufferDesc.Height) ? runtime->mDefaultDepthStencil : nullptr);
			device->RSSetViewports(1, &pass.Viewport);

			for (UINT target = 0; target < D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT; ++target)
			{
				if (pass.RT[target] == nullptr)
				{
					continue;
				}

				const FLOAT color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				device->ClearRenderTargetView(pass.RT[target], color);
			}

			// Draw triangle
			device->Draw(3, 0);

			const_cast<D3D10Runtime *>(runtime.get())->Runtime::OnDraw(3);

			// Reset shader resources
			ID3D10ShaderResourceView *null[D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = { nullptr };
			device->VSSetShaderResources(0, static_cast<UINT>(pass.SRV.size()), null);
			device->PSSetShaderResources(0, static_cast<UINT>(pass.SRV.size()), null);

			// Reset rendertargets
			device->OMSetRenderTargets(0, nullptr, nullptr);

			// Update shader resources
			for (ID3D10ShaderResourceView *srv : pass.RTSRV)
			{
				if (srv == nullptr)
				{
					continue;
				}

				D3D10_SHADER_RESOURCE_VIEW_DESC srvdesc;
				srv->GetDesc(&srvdesc);

				if (srvdesc.Texture2D.MipLevels > 1)
				{
					device->GenerateMips(srv);
				}
			}
		}
	}
}