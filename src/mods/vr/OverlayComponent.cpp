#define NOMINMAX

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

#include <glm/gtx/intersect.hpp>
#include <imgui_internal.h>

#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler")

#include "Framework.hpp"
#include "../VR.hpp"
#include "../utility/ImGui.hpp"
#include "d3d12/CommandContext.hpp"
#include "utility/Logging.hpp"

#include "OverlayComponent.hpp"

namespace vrmod {
using Microsoft::WRL::ComPtr;

void OverlayComponent::on_reset() {
    m_overlay_data = {};
    m_openxr.reset_daysgone_ahud_pose();
}

std::optional<std::string> OverlayComponent::on_initialize_openvr() {
    m_overlay_data = {};

    auto create_overlay = [this](const std::string& name, float size, vr::VROverlayHandle_t& out) -> std::optional<std::string> {
        // create vr overlay
        auto overlay_error = vr::VROverlay()->CreateOverlay(name.c_str(), name.c_str(), &out);

        if (overlay_error != vr::VROverlayError_None) {
            return "VROverlay failed to create overlay: " + std::string{vr::VROverlay()->GetOverlayErrorNameFromEnum(overlay_error)};
        }

        // set overlay to visible
        vr::VROverlay()->ShowOverlay(out);

        overlay_error = vr::VROverlay()->SetOverlayWidthInMeters(out, size);

        if (overlay_error != vr::VROverlayError_None) {
            return "VROverlay failed to set overlay width: " + std::string{vr::VROverlay()->GetOverlayErrorNameFromEnum(overlay_error)};
        }

        // same thing as above but absolute instead
        // get absolute tracking pose of hmd with GetDeviceToAbsoluteTrackingPose
        // then get the matrix from that
        // then set it as the overlay transform
        vr::TrackedDevicePose_t pose{};
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, &pose, 1);
        vr::VROverlay()->SetOverlayTransformAbsolute(out, vr::TrackingUniverseStanding, &pose.mDeviceToAbsoluteTracking);

        // set overlay flag to receive smooth scroll events
        overlay_error = vr::VROverlay()->SetOverlayFlag(out, vr::VROverlayFlags::VROverlayFlags_SendVRSmoothScrollEvents, true);

        if (overlay_error != vr::VROverlayError_None) {
            return "VROverlay failed to set overlay flag: " + std::string{vr::VROverlay()->GetOverlayErrorNameFromEnum(overlay_error)};
        }

        spdlog::info("Made overlay with handle {}", out);
        return std::nullopt;
    };

    if (!create_overlay("REFramework", 0.25f, m_overlay_handle)) {
        auto overlay_error = vr::VROverlay()->SetOverlayInputMethod(m_overlay_handle, vr::VROverlayInputMethod_Mouse);

        if (overlay_error != vr::VROverlayError_None) {
            return "VROverlay failed to set overlay input method: " + std::string{vr::VROverlay()->GetOverlayErrorNameFromEnum(overlay_error)};
        }
    }

    if (!create_overlay("Slate", 2.0f, m_slate_overlay_handle)) {
        auto overlay_error = vr::VROverlay()->SetOverlayInputMethod(m_slate_overlay_handle, vr::VROverlayInputMethod_None);

        if (overlay_error != vr::VROverlayError_None) {
            return "VROverlay failed to set overlay input method: " + std::string{vr::VROverlay()->GetOverlayErrorNameFromEnum(overlay_error)};
        }
    }

    // Path of Titans procedural vignette overlays: own CPU gradient texture,
    // hidden until their drivers raise alpha above zero.
    static constexpr const char* s_pot_vignette_names[POT_VIGNETTE_SLOT_COUNT] {
        "PotVignetteRed",
        "PotVignetteGreen",
        "PotVignetteBlue",
        "PotVignetteMotion",
    };

    for (auto i = 0; i < POT_VIGNETTE_SLOT_COUNT; ++i) {
        if (!create_overlay(s_pot_vignette_names[i], m_pot_vignette_size->value(), m_pot_vignette_handles[i])) {
            auto overlay_error = vr::VROverlay()->SetOverlayInputMethod(m_pot_vignette_handles[i], vr::VROverlayInputMethod_None);

            if (overlay_error != vr::VROverlayError_None) {
                return "VROverlay failed to set vignette input method: " + std::string{vr::VROverlay()->GetOverlayErrorNameFromEnum(overlay_error)};
            }
        }

        vr::VROverlay()->HideOverlay(m_pot_vignette_handles[i]);
        m_pot_vignette_alphas[i].store(0.0f, std::memory_order_relaxed);
    }

    return std::nullopt;
}

void OverlayComponent::on_pre_imgui_frame() {
    this->update_input_openvr();
    this->update_input_mouse_emulation();
}

void OverlayComponent::update_input_mouse_emulation() {
    if (m_forced_aim) {
        VR::get()->set_aim_allowed(true);
        m_forced_aim = false;
    }

    if (VR::get()->get_runtime()->is_openvr() && m_framework_wrist_ui->value()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_mouse_move_time).count();
    const auto delta_f = std::min<float>(delta / 1000.0f, 0.5f);
    m_last_mouse_move_time = now;

    if (!m_framework_mouse_emulation->value()) {
        return;
    }

    if (m_framework_intersect_state.intersecting && VR::get()->is_using_controllers()) {
        auto vr = VR::get();
        auto& io = ImGui::GetIO();
        const auto is_initial_frame = !vr->is_using_afr() || vr->get_frame_count() % 2 == vr->m_left_eye_interval;

        const auto x = m_framework_intersect_state.swapchain_intersection_point.x;
        const auto y = m_framework_intersect_state.swapchain_intersection_point.y;
        
        const auto window_size = g_framework->get_last_window_size();
        const auto window_pos = g_framework->get_last_window_pos();

        // lerp towards the intersection point
        m_last_mouse_pos = glm::lerp(m_last_mouse_pos, glm::vec2{x, y}, delta_f * 10.0f);

        if (imgui::is_point_intersecting_any(m_last_mouse_pos.x, m_last_mouse_pos.y)) {
            io.MousePos = ImVec2{
                m_last_mouse_pos.x,
                m_last_mouse_pos.y
            };

            static bool was_pressing = false;
            static bool was_pressing_right = false;

            // Left click.
            const auto& a_button_right = !vr->m_swap_controllers->value() ? VR::s_action_a_button_right : VR::s_action_a_button_left;
            if (VR::get()->is_action_active_any_joystick(vr->get_action_handle(a_button_right))) {
                // Clear any gamepad A events.
                auto ctx = io.Ctx;

                if (ctx != nullptr) {
                    for (auto i = 0; i < ctx->InputEventsQueue.size(); ++i) {
                        auto& event = ctx->InputEventsQueue[i];

                        if (event.Type == ImGuiInputEventType::ImGuiInputEventType_Key && event.Key.Key == ImGuiKey_GamepadFaceDown) {
                            ctx->InputEventsQueue.erase(ctx->InputEventsQueue.begin() + i);
                            --i;
                        }
                    }
                }

                io.AddMouseButtonEvent(0, true);
                was_pressing = true;
            } else if (was_pressing) {
                io.AddMouseButtonEvent(0, false);
                was_pressing = false;
            }

            // Right click.
            const auto& b_button_right = !vr->m_swap_controllers->value() ? VR::s_action_b_button_right : VR::s_action_b_button_left;
            if (VR::get()->is_action_active_any_joystick(vr->get_action_handle(b_button_right))) {
                // Clear any gamepad B events.
                auto ctx = io.Ctx;

                if (ctx != nullptr) {
                    for (auto i = 0; i < ctx->InputEventsQueue.size(); ++i) {
                        auto& event = ctx->InputEventsQueue[i];

                        if (event.Type == ImGuiInputEventType::ImGuiInputEventType_Key && event.Key.Key == ImGuiKey_GamepadFaceLeft) {
                            ctx->InputEventsQueue.erase(ctx->InputEventsQueue.begin() + i);
                            --i;
                        }
                    }
                }

                io.AddMouseButtonEvent(1, true);
                was_pressing_right = true;
            } else if (was_pressing_right) {
                io.AddMouseButtonEvent(1, false);
                was_pressing_right = false;
            }

            const auto right_stick_axis = vr->get_right_stick_axis();
            
            // Mousewheel
            if (right_stick_axis.y > 0.5f) {
                io.MouseWheel += right_stick_axis.y * delta_f * 10.0f;
            } else if (right_stick_axis.y < -0.5f) {
                io.MouseWheel += right_stick_axis.y * delta_f * 10.0f;
            }

            VR::get()->set_aim_allowed(false);
            m_forced_aim = true;
        } else if (m_forced_aim) {
            VR::get()->set_aim_allowed(true);
            m_forced_aim = false;
        }
    }
}

void OverlayComponent::on_post_compositor_submit() {
    this->update_overlay_openvr();
    this->update_slate_openvr();
    this->update_pot_vignette_pool_openvr();
}


void OverlayComponent::on_config_save(utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
}

void OverlayComponent::on_config_load(const utility::Config& cfg, bool set_defaults) {
    for (IModValue& option : m_options) {
        option.config_load(cfg, set_defaults);
    }
}

void OverlayComponent::on_draw_ui() {
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNode("Overlay Options")) {
        if (VR::get()->get_runtime()->is_cylinder_layer_allowed()) {
            m_slate_overlay_type->draw("Overlay Type");

            if ((OverlayType)m_slate_overlay_type->value() == OverlayType::CYLINDER)  {
                m_slate_cylinder_angle->draw("UI Cylinder Angle");
            }
        }

        float ui_offset[] { m_slate_x_offset->value(), m_slate_y_offset->value(), m_slate_distance->value() };

        if (ImGui::SliderFloat3("UI Offset", ui_offset, -10.0f, 10.0f)) {
            m_slate_x_offset->value() = ui_offset[0];
            m_slate_y_offset->value() = ui_offset[1];
            m_slate_distance->value() = ui_offset[2];
        }

        m_slate_distance->draw("UI Distance");
        m_slate_size->draw("UI Size");
        m_ui_follows_view->draw("UI Follows View");
        m_ui_invert_alpha->draw("UI Invert Alpha");

        m_framework_distance->draw("Framework Distance");
        m_framework_size->draw("Framework Size");
        m_framework_ui_follows_view->draw("Framework Follows View");
        if (VR::get()->get_runtime()->is_openvr()) {
            ImGui::SameLine();
            m_framework_wrist_ui->draw("Framework Wrist UI");
        }
        m_framework_mouse_emulation->draw("Framework Mouse Emulation");
        ImGui::TreePop();
    }
}

void OverlayComponent::update_input_openvr() {
    if (!VR::get()->get_runtime()->is_openvr()) {
        return;
    }

    if (!m_framework_wrist_ui->value()) {
        return;
    }

    auto vr = VR::get();
    auto& io = ImGui::GetIO();
    const auto is_initial_frame = !vr->is_using_afr() || vr->get_frame_count() % 2 == vr->m_left_eye_interval;

    // Restore the previous frame's input state
    memcpy(io.KeysDown, m_initial_imgui_input_state.KeysDown, sizeof(io.KeysDown));
    memcpy(io.MouseDown, m_initial_imgui_input_state.MouseDown, sizeof(io.MouseDown));
    io.MousePos = m_initial_imgui_input_state.MousePos;
    io.MouseWheel = m_initial_imgui_input_state.MouseWheel;
    io.MouseWheelH = m_initial_imgui_input_state.MouseWheelH;
    io.KeyCtrl = m_initial_imgui_input_state.KeyCtrl;
    io.KeyShift = m_initial_imgui_input_state.KeyShift;
    io.KeyAlt = m_initial_imgui_input_state.KeyAlt;
    io.KeySuper = m_initial_imgui_input_state.KeySuper;

    if (!is_initial_frame) {
        return;
    }

    io.MouseWheel = 0;
    io.MouseWheelH = 0;

    const auto is_d3d11 = g_framework->get_renderer_type() == Framework::RendererType::D3D11;

    const auto last_window_pos = g_framework->get_last_window_pos();
    const auto last_window_size = g_framework->get_last_window_size();
    const auto rendertarget_width = is_d3d11 ? g_framework->get_rendertarget_width_d3d11() : g_framework->get_rendertarget_width_d3d12();
    const auto rendertarget_height = is_d3d11 ? g_framework->get_rendertarget_height_d3d11() : g_framework->get_rendertarget_height_d3d12();

    // Poll overlay events
    vr::VREvent_t event{};
    const auto hwnd = g_framework->get_window();

    while (vr::VROverlay()->PollNextOverlayEvent(m_overlay_handle, &event, sizeof(event))) {
        switch (event.eventType) {
            case vr::VREvent_MouseButtonDown:
                m_initial_imgui_input_state.MouseDown[0] = true;
                io.MouseDown[0] = true;
                break;
            case vr::VREvent_MouseButtonUp:
                m_initial_imgui_input_state.MouseDown[0] = false;
                io.MouseDown[0] = false;
                break;
            case vr::VREvent_MouseMove: {
                const std::array<float, 2> raw_coords { event.data.mouse.x, event.data.mouse.y };

                // Convert from GL space (bottom left is 0,0) to window space (top left is 0,0)
                const auto mouse_point = ImVec2{
                    raw_coords[0],
                    (rendertarget_height - raw_coords[1])
                };

                // override imgui mouse position
                m_initial_imgui_input_state.MousePos = mouse_point;
                io.MousePos = mouse_point;
            } break;
            case vr::VREvent_ScrollSmooth: {
                m_initial_imgui_input_state.MouseWheelH += event.data.scroll.xdelta;
                m_initial_imgui_input_state.MouseWheel += event.data.scroll.ydelta;
                io.MouseWheelH = event.data.scroll.xdelta;
                io.MouseWheel = event.data.scroll.ydelta;
            } break;
            default:
                break;
        }
    }
}

void OverlayComponent::update_slate_openvr() {
    auto vr = VR::get();

    if (!vr->get_runtime()->is_openvr()) {
        return;
    }

    if (!vr->is_gui_enabled()) {
        vr::VROverlay()->ClearOverlayTexture(m_slate_overlay_handle);
        return;
    }

    const auto is_d3d11 = g_framework->get_renderer_type() == Framework::RendererType::D3D11;

    // TODO: do the sizing / scaling calculations below need to take into account non-standard VRTextureBounds_t
    // when we force a symmetrical eye projection matrix?
    vr::VRTextureBounds_t bounds{};
    bounds.uMin = 0.0f;
    bounds.uMax = 1.0f;
    bounds.vMin = 0.0f;
    bounds.vMax = 1.0f;

    vr::VROverlay()->SetOverlayTextureBounds(m_slate_overlay_handle, &bounds);

    vr::TrackedDevicePose_t pose{};
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, &pose, 1);

    auto rotation_offset = glm::inverse(vr->get_rotation_offset());

    if (vr->is_decoupled_pitch_enabled() && vr->is_decoupled_pitch_ui_adjust_enabled()) {
        const auto pre_flat_rotation = vr->get_pre_flattened_rotation();
        const auto pre_flat_pitch = utility::math::pitch_only(pre_flat_rotation);

        // Add the inverse of the pitch rotation to the rotation offset
        rotation_offset = glm::normalize(glm::inverse(pre_flat_pitch * vr->get_rotation_offset()));
    }

    //auto glm_matrix = glm::rowMajor4(Matrix4x4f{*(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking});
    auto glm_matrix = Matrix4x4f{rotation_offset};
    if (m_ui_follows_view->value()) {
        const auto mat = glm::rowMajor4(Matrix4x4f{*(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking});
        glm_matrix = glm::extractMatrixRotation(mat);
        glm_matrix[3] += mat[3];
    } else {
        glm_matrix[3] += vr->get_standing_origin();
    }

    glm_matrix[3] -= glm_matrix[2] * m_slate_distance->value();
    glm_matrix[3] += m_slate_x_offset->value() * glm_matrix[0];
    glm_matrix[3] += m_slate_y_offset->value() * glm_matrix[1];
    glm_matrix[3].w = 1.0f;
    
    const auto steamvr_matrix = Matrix3x4f{glm::rowMajor4(glm_matrix)};
    vr::VROverlay()->SetOverlayTransformAbsolute(m_slate_overlay_handle, vr::TrackingUniverseStanding, (vr::HmdMatrix34_t*)&steamvr_matrix);

    const auto is_d3d12 = g_framework->get_renderer_type() == Framework::RendererType::D3D12;
    const auto size = is_d3d12 ? g_framework->get_d3d12_rt_size() : g_framework->get_d3d11_rt_size();
    const auto aspect = size.x / size.y;
    const auto width_meters = m_slate_size->value() * aspect;
    vr::VROverlay()->SetOverlayWidthInMeters(m_slate_overlay_handle, width_meters);

    if (is_d3d11) {
        if (vr->m_d3d11.get_ui_tex().Get() == nullptr) {
            return;
        }

        vr::Texture_t ui_tex{(void*)vr->m_d3d11.get_ui_tex().Get(), vr::TextureType_DirectX, vr::ColorSpace_Auto};
        vr::VROverlay()->SetOverlayTexture(m_slate_overlay_handle, &ui_tex);   
    } else {
        if (vr->m_d3d12.get_openvr_ui_tex().texture.Get() == nullptr) {
            return;
        }

        vr::D3D12TextureData_t overlay_tex {
            vr->m_d3d12.get_openvr_ui_tex().texture.Get(),
            g_framework->get_d3d12_hook()->get_command_queue(),
            0
        };

        vr::Texture_t ui_tex{(void*)&overlay_tex, vr::TextureType_DirectX12, vr::ColorSpace_Auto};
        vr::VROverlay()->SetOverlayTexture(m_slate_overlay_handle, &ui_tex);   
    }
}

bool OverlayComponent::update_wrist_overlay_openvr() {
    if (!VR::get()->get_runtime()->is_openvr()) {
        return false;
    }

    auto& vr = VR::get();

    if (!m_framework_wrist_ui->value()) {
        return true;
    }

    const auto is_d3d11 = g_framework->get_renderer_type() == Framework::RendererType::D3D11;

    // update REFramework menu overlay
    const auto last_window_pos = g_framework->get_last_window_pos();
    const auto last_window_size = g_framework->get_last_window_size();
    const auto render_target_width = is_d3d11 ? g_framework->get_rendertarget_width_d3d11() : g_framework->get_rendertarget_width_d3d12();
    const auto render_target_height = is_d3d11 ? g_framework->get_rendertarget_height_d3d11() : g_framework->get_rendertarget_height_d3d12();

    // only update certain parts of the overlay
    // if things like the width or position of the window change
    if (m_overlay_data.last_x != last_window_pos.x || m_overlay_data.last_y != last_window_pos.y ||
        m_overlay_data.last_width != last_window_size.x || m_overlay_data.last_height != last_window_size.y ||
        m_overlay_data.last_render_target_width != render_target_width || m_overlay_data.last_render_target_height != render_target_height || m_just_closed_ui || m_just_opened_ui) 
    {
        // scaling for the intersection mask
        // so it doesn't become too intrusive during gameplay
        const auto scale = m_closed_ui ? 0.25f : 1.0f;

        // TODO: do the sizing / scaling calculations below need to take into account non-standard VRTextureBounds_t
        // when we force a symmetrical eye projection matrix?
        vr::VRTextureBounds_t bounds{};
        bounds.uMin = last_window_pos.x / render_target_width ;
        bounds.uMax = (last_window_pos.x + last_window_size.x) / render_target_width;
        bounds.vMin = last_window_pos.y / render_target_height;
        bounds.vMax = (last_window_pos.y + last_window_size.y) / render_target_height;

        vr::VROverlay()->SetOverlayTextureBounds(m_overlay_handle, &bounds);

        // necessary, fixes all sorts of issues with ray intersection
        const auto mouse_scale = vr::HmdVector2_t{(float)render_target_width, (float)render_target_height};
        vr::VROverlay()->SetOverlayMouseScale(m_overlay_handle, &mouse_scale);

        vr::VROverlayIntersectionMaskPrimitive_t intersection_mask{};

        intersection_mask.m_nPrimitiveType = vr::EVROverlayIntersectionMaskPrimitiveType::OverlayIntersectionPrimitiveType_Rectangle;
        intersection_mask.m_Primitive.m_Rectangle.m_flTopLeftX = last_window_pos.x;
        intersection_mask.m_Primitive.m_Rectangle.m_flTopLeftY = last_window_pos.y;
        intersection_mask.m_Primitive.m_Rectangle.m_flWidth = last_window_size.x;
        intersection_mask.m_Primitive.m_Rectangle.m_flHeight = last_window_size.y;

        vr::VROverlay()->SetOverlayIntersectionMask(m_overlay_handle, &intersection_mask, 1);

        // and now set the last known values
        m_overlay_data.last_x = last_window_pos.x;
        m_overlay_data.last_y = last_window_pos.y;
        m_overlay_data.last_width = last_window_size.x;
        m_overlay_data.last_height = last_window_size.y;
        m_overlay_data.last_render_target_width = render_target_width;
        m_overlay_data.last_render_target_height = render_target_height;
    }

    // Fire an intersection test and enable the laser pointer if we're intersecting
    const auto& controllers = vr->get_controllers();

    bool should_show_overlay = !m_closed_ui;

    if (controllers.size() >= 2 && !vr->is_any_action_down()) {
        Matrix4x4f left_controller_world_transform{glm::identity<Matrix4x4f>()};

        // Attach the overlay to the left controller
        if (controllers[0] != vr::k_unTrackedDeviceIndexInvalid) {
            const auto position_offset = vr->m_overlay_position;
            const auto rotation_offset = vr->m_overlay_rotation;
 
            left_controller_world_transform = vr->get_transform(controllers[0]) * Matrix4x4f{glm::quat{rotation_offset}};
            left_controller_world_transform[3] -= glm::extractMatrixRotation(left_controller_world_transform) * position_offset;
            left_controller_world_transform[3].w = 1.0f;

            const auto steamvr_transform = Matrix3x4f{ glm::rowMajor4(left_controller_world_transform) };
            
            vr::VROverlay()->SetOverlayTransformAbsolute(m_overlay_handle, vr::ETrackingUniverseOrigin::TrackingUniverseStanding, (vr::HmdMatrix34_t*)&steamvr_transform);
        }

        bool any_intersected = false;

        for (const auto& controller_index : controllers) {
            const auto is_left = controller_index == controllers[0];

            vr::VRInputValueHandle_t controller_handle{};

            if (is_left) {
                controller_handle = vr->m_left_joystick;
            } else {
                controller_handle = vr->m_right_joystick;
            }

            char render_name[vr::k_unMaxPropertyStringSize]{};
            vr::VRSystem()->GetStringTrackedDeviceProperty(controller_index, vr::Prop_RenderModelName_String, render_name, vr::k_unMaxPropertyStringSize);

            vr::RenderModel_ControllerMode_State_t controller_state{};
            vr::RenderModel_ComponentState_t component_state{};

            // get tip component state
            if (!vr::VRRenderModels()->GetComponentStateForDevicePath(render_name, vr::k_pch_Controller_Component_Tip, controller_handle, &controller_state, &component_state)) {
                continue;
            }

            // obtain tip world transform
            const auto controller_world_transform = vr->get_transform(controller_index);
            const auto tip_local_transform = glm::rowMajor4(Matrix4x4f{*(Matrix3x4f*)&component_state.mTrackingToComponentLocal});
            const auto tip_world_transform = controller_world_transform * tip_local_transform;

            // Set up intersection data
            vr::VROverlayIntersectionResults_t intersection_results{};
            vr::VROverlayIntersectionParams_t intersection_params{};
            intersection_params.eOrigin = vr::TrackingUniverseOrigin::TrackingUniverseStanding;
            intersection_params.vSource.v[0] = tip_world_transform[3][0];
            intersection_params.vSource.v[1] = tip_world_transform[3][1];
            intersection_params.vSource.v[2] = tip_world_transform[3][2];
            intersection_params.vDirection.v[0] = -tip_world_transform[2][0];
            intersection_params.vDirection.v[1] = -tip_world_transform[2][1];
            intersection_params.vDirection.v[2] = -tip_world_transform[2][2];

            // Do the intersection test
            if (vr::VROverlay()->ComputeOverlayIntersection(m_overlay_handle, &intersection_params, &intersection_results)) {
                auto normal = Vector4f{intersection_results.vNormal.v[0], intersection_results.vNormal.v[1], intersection_results.vNormal.v[2], 1.0f};
                normal = glm::inverse(glm::extractMatrixRotation(tip_world_transform)) * normal;

                if (m_closed_ui) {
                    const auto u = ((intersection_results.vUVs.v[0] * m_overlay_data.last_render_target_width) - m_overlay_data.last_x) / m_overlay_data.last_width;
                    const auto v = ((m_overlay_data.last_render_target_height - (intersection_results.vUVs.v[1] * m_overlay_data.last_render_target_height)) - m_overlay_data.last_y) / m_overlay_data.last_height;

                    any_intersected = u >= 0.25f &&
                                    u <= 0.75f && 
                                    v >= 0.25f && 
                                    v <= 0.75f;

                    // Make sure the intersection hit the front of the overlay, not the back
                    any_intersected = any_intersected && normal.z > 0.0f;
                } else {
                    any_intersected = normal.z > 0.0f;
                }
            }
        }

        // Do the same intersection test with the user's view/head gaze
        // Both the head and controller needs to be aimed at the overlay for it to appear
        if (any_intersected) {
            auto head_world_transform = vr->get_transform(vr::k_unTrackedDeviceIndex_Hmd);
            vr::VROverlayIntersectionResults_t intersection_results{};
            vr::VROverlayIntersectionParams_t intersection_params{};
            intersection_params.eOrigin = vr::TrackingUniverseOrigin::TrackingUniverseStanding;
            intersection_params.vSource.v[0] = head_world_transform[3][0];
            intersection_params.vSource.v[1] = head_world_transform[3][1];
            intersection_params.vSource.v[2] = head_world_transform[3][2];
            intersection_params.vDirection.v[0] = -head_world_transform[2][0];
            intersection_params.vDirection.v[1] = -head_world_transform[2][1];
            intersection_params.vDirection.v[2] = -head_world_transform[2][2];

            if (vr::VROverlay()->ComputeOverlayIntersection(m_overlay_handle, &intersection_params, &intersection_results)) {
                if (m_closed_ui) {
                    const auto u = ((intersection_results.vUVs.v[0] * m_overlay_data.last_render_target_width) - m_overlay_data.last_x) / m_overlay_data.last_width;
                    const auto v = ((m_overlay_data.last_render_target_height - (intersection_results.vUVs.v[1] * m_overlay_data.last_render_target_height)) - m_overlay_data.last_y) / m_overlay_data.last_height;

                    any_intersected = u >= 0.25f &&
                                    u <= 0.75f && 
                                    v >= 0.25f && 
                                    v <= 0.75f;
                }

                auto normal = Vector4f{intersection_results.vNormal.v[0], intersection_results.vNormal.v[1], intersection_results.vNormal.v[2], 1.0f};
                normal = glm::inverse(glm::extractMatrixRotation(head_world_transform)) * normal;

                // Make sure the intersection hit the front of the overlay, not the back
                any_intersected = any_intersected && normal.z > 0.0f;
            } else {
                any_intersected = false;
            }
        }

        // set overlay flag
        if (any_intersected) {
            should_show_overlay = true;
            vr::VROverlay()->SetOverlayFlag(m_overlay_handle, vr::VROverlayFlags::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);

            g_framework->set_draw_ui(true);

            if (m_closed_ui) {
                m_just_opened_ui = true;
            } else {
                m_just_opened_ui = false;
            }

            m_closed_ui = false;
            m_just_closed_ui = false;
        } else {
            should_show_overlay = false;
            vr::VROverlay()->SetOverlayFlag(m_overlay_handle, vr::VROverlayFlags::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);

            if (!m_closed_ui) {
                g_framework->set_draw_ui(false);

                m_closed_ui = true;
                m_just_closed_ui = true;
            } else {
                m_just_closed_ui = false;
            }

            m_just_opened_ui = false;
        }
    }

    return should_show_overlay;
}

void OverlayComponent::update_overlay_openvr() {
    if (!VR::get()->get_runtime()->is_openvr()) {
        return;
    }

    auto& vr = VR::get();

    const auto is_d3d11 = g_framework->get_renderer_type() == Framework::RendererType::D3D11;

    bool should_show_overlay = update_wrist_overlay_openvr();

    if (m_framework_wrist_ui->value()) {
        vr::VROverlay()->ShowOverlay(m_overlay_handle); // always show overlay idk look at it later

        if (should_show_overlay) {
            // finally set the texture
            if (is_d3d11) {
                vr::Texture_t imgui_tex{(void*)g_framework->get_rendertarget_d3d11().Get(), vr::TextureType_DirectX, vr::ColorSpace_Auto};
                vr::VROverlay()->SetOverlayTexture(m_overlay_handle, &imgui_tex);   
            } else {
                auto& hook = g_framework->get_d3d12_hook();

                vr::D3D12TextureData_t texture_data {
                    g_framework->get_rendertarget_d3d12().Get(),
                    hook->get_command_queue(),
                    0
                };
                
                vr::Texture_t imgui_tex{(void*)&texture_data, vr::TextureType_DirectX12, vr::ColorSpace_Auto};
                vr::VROverlay()->SetOverlayTexture(m_overlay_handle, &imgui_tex);
            }
        } else {
            if (is_d3d11) {
                // draw a blank texture (don't just call HideOverlay, we'll no longer be able to use intersection tests)
                vr::Texture_t imgui_tex{(void*)g_framework->get_blank_rendertarget_d3d11().Get(), vr::TextureType_DirectX, vr::ColorSpace_Auto};
                vr::VROverlay()->SetOverlayTexture(m_overlay_handle, &imgui_tex);
            } else {
                auto& hook = g_framework->get_d3d12_hook();

                vr::D3D12TextureData_t texture_data {
                    g_framework->get_blank_rendertarget_d3d12().Get(),
                    hook->get_command_queue(),
                    0
                };
                
                vr::Texture_t imgui_tex{(void*)&texture_data, vr::TextureType_DirectX12, vr::ColorSpace_Auto};
                vr::VROverlay()->SetOverlayTexture(m_overlay_handle, &imgui_tex);
            }
        }

        return;
    }

    // Draw the UI as a plane in front of the user instead
    if (!m_framework_wrist_ui->value() && g_framework->is_drawing_anything()) {
        vr::VROverlay()->ShowOverlay(m_overlay_handle);

        // Show the entire texture
        // TODO: do the sizing / scaling calculations below need to take into account non-standard VRTextureBounds_t
        // when we force a symmetrical eye projection matrix?
        vr::VRTextureBounds_t bounds{};
        bounds.uMin = 0.0f;
        bounds.uMax = 1.0f;
        bounds.vMin = 0.0f;
        bounds.vMax = 1.0f;

        vr::VROverlay()->SetOverlayTextureBounds(m_overlay_handle, &bounds);

        vr::TrackedDevicePose_t pose{};
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, &pose, 1);

        //auto glm_matrix = glm::rowMajor4(Matrix4x4f{*(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking});
        auto rotation_offset = glm::inverse(vr->get_rotation_offset());

        // If we're not drawing the UI, this means we want to draw the cursor all the time
        // So we need to rotate the UI's pitch as well
        if (!g_framework->is_drawing_ui() && vr->is_decoupled_pitch_enabled() && vr->is_decoupled_pitch_ui_adjust_enabled()) {
            const auto pre_flat_rotation = vr->get_pre_flattened_rotation();
            const auto pre_flat_pitch = utility::math::pitch_only(pre_flat_rotation);

            // Add the inverse of the pitch rotation to the rotation offset
            rotation_offset = glm::normalize(glm::inverse(pre_flat_pitch * vr->get_rotation_offset()));
        }

        auto glm_matrix = Matrix4x4f{rotation_offset};

        if (m_framework_ui_follows_view->value()) {
            // todo
        }

        glm_matrix[3] += vr->get_standing_origin();

        if (g_framework->is_drawing_ui()) {
            glm_matrix[3] -= glm_matrix[2] * m_framework_distance->value();
        } else {
            glm_matrix[3] -= glm_matrix[2] * (m_slate_distance->value() - 0.01f);

            glm_matrix[3] += m_slate_x_offset->value() * glm_matrix[0];
            glm_matrix[3] += m_slate_y_offset->value() * glm_matrix[1];
        }

        glm_matrix[3].w = 1.0f;
        const auto steamvr_matrix = Matrix3x4f{glm::rowMajor4(glm_matrix)};
        vr::VROverlay()->SetOverlayTransformAbsolute(m_overlay_handle, vr::TrackingUniverseStanding, (vr::HmdMatrix34_t*)&steamvr_matrix);

        const auto is_d3d12 = g_framework->get_renderer_type() == Framework::RendererType::D3D12;
        const auto size = is_d3d12 ? g_framework->get_d3d12_rt_size() : g_framework->get_d3d11_rt_size();
        const auto aspect = size.x / size.y;
        const auto size_meters = g_framework->is_drawing_ui() ? m_framework_size->value() : m_slate_size->value();

        const float scale_factor = g_framework->is_drawing_ui() ? (size.x / 1920.0f) : 1.0f;
        const float adjusted_size_meters = size_meters * scale_factor;

        const auto width_meters = adjusted_size_meters * aspect;
        const auto height_meters = adjusted_size_meters;

        vr::VROverlay()->SetOverlayWidthInMeters(m_overlay_handle, width_meters);

        if (is_d3d11) {
            vr::Texture_t imgui_tex{(void*)g_framework->get_rendertarget_d3d11().Get(), vr::TextureType_DirectX, vr::ColorSpace_Auto};
            vr::VROverlay()->SetOverlayTexture(m_overlay_handle, &imgui_tex);   
        } else {
            auto& hook = g_framework->get_d3d12_hook();

            vr::D3D12TextureData_t texture_data {
                g_framework->get_rendertarget_d3d12().Get(),
                hook->get_command_queue(),
                0
            };
            
            vr::Texture_t imgui_tex{(void*)&texture_data, vr::TextureType_DirectX12, vr::ColorSpace_Auto};
            vr::VROverlay()->SetOverlayTexture(m_overlay_handle, &imgui_tex);
        }

        // Check if the controller pointer intersects with the quad, and we can use this to emulate the mouse
        if (vr->is_using_controllers()) {
            // Right only for now for testing
            const auto controller_index = !vr->m_swap_controllers->value() ? vr->get_right_controller_index() : vr->get_left_controller_index();
            const auto right_controller_rot = glm::quat{vr->get_rotation(controller_index, false)};
            const auto right_controller_pos = glm::vec3{vr->get_position(controller_index, false)};

            const auto start = right_controller_pos;
            auto fwd = (right_controller_rot * glm::vec3{0.0f, 0.0f, -1.0f});
            const auto end = right_controller_pos + (fwd * 1000.0f);
            
            const auto plane_pos = glm::vec3{glm_matrix[3]};

            float intersection_distance = 0.0f;

            auto& intersect_state = g_framework->is_drawing_ui() ? m_framework_intersect_state : m_intersect_state;
            auto& other_intersect_state = g_framework->is_drawing_ui() ? m_intersect_state : m_framework_intersect_state;
            other_intersect_state.intersecting = false;
            if (glm::intersectRayPlane<glm::vec3>(start, fwd, plane_pos, glm::normalize(glm::vec3{glm_matrix[2]}), intersection_distance)) {
                const auto intersection_point = start + (fwd * intersection_distance);

                const auto local_point = glm::inverse(glm_matrix) * glm::vec4{intersection_point, 1.0f};

                const auto w_half = width_meters / 2.0f;
                const auto h_half = height_meters / 2.0f;

                if (local_point.x >= -w_half && local_point.x <= w_half && local_point.y >= -h_half && local_point.y <= h_half) {
                    const auto x = (local_point.x + w_half) / width_meters;
                    const auto y = (height_meters - (local_point.y + h_half)) / height_meters;

                    intersect_state.quad_intersection_point = {x, y};
                    intersect_state.intersecting = true;

                    /*if (auto it = vr->m_openxr->swapchains.find((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI); it != vr->m_openxr->swapchains.end()) {
                        const auto client_x = (int32_t)((float)it->second.width * x);
                        const auto client_y = (int32_t)((float)it->second.height * y);

                        intersect_state.swapchain_intersection_point = {client_x, client_y};
                    }*/
                    // Change to openvr version
                    const auto client_x = (int32_t)(size.x * x);
                    const auto client_y = (int32_t)(size.y * y);

                    intersect_state.swapchain_intersection_point = {client_x, client_y};
                } else {
                    intersect_state.intersecting = false;
                }
            }
        } else {
            m_framework_intersect_state.intersecting = false;
            m_intersect_state.intersecting = false;
        }
    } else {
        vr::VROverlay()->ClearOverlayTexture(m_overlay_handle);
        vr::VROverlay()->HideOverlay(m_overlay_handle);
    }
}

namespace {
glm::quat make_slate_stage_rotation(VR* vr, const glm::quat& rotation_offset, const glm::quat& pre_flattened_rotation) {
    if (vr->is_decoupled_pitch_enabled() && vr->is_decoupled_pitch_ui_adjust_enabled()) {
        const auto pre_flat_pitch = utility::math::pitch_only(pre_flattened_rotation);
        return glm::normalize(glm::inverse(pre_flat_pitch * rotation_offset));
    }

    return glm::normalize(glm::inverse(rotation_offset));
}

const char* get_ui_layer_pose_refusal_reason(const UILayerPoseBasis* pose_basis, bool follows_view, bool stabilizer_enabled) {
    if (follows_view) {
        return "view_space";
    }

    if (!stabilizer_enabled) {
        return "disabled";
    }

    if (pose_basis == nullptr) {
        return "no_basis";
    }

    if (!pose_basis->valid) {
        return "invalid_basis";
    }

    if (!pose_basis->stabilizer_allowed) {
        return "not_allowed";
    }

    return "none";
}
}

std::optional<std::reference_wrapper<XrCompositionLayerQuad>> OverlayComponent::OpenXR::generate_slate_quad(
    runtimes::OpenXR::SwapchainIndex swapchain, 
    XrEyeVisibility eye,
    const UILayerPoseBasis* pose_basis,
    bool force_stage_space)
{
    auto& vr = VR::get();

    if (!vr->is_gui_enabled()) {
        m_parent->m_intersect_state.intersecting = false;
        return std::nullopt;
    }

    if (!vr->m_openxr->swapchains.contains((uint32_t)swapchain)) {
        return std::nullopt;
    }

    const auto is_left_eye = eye == XR_EYE_VISIBILITY_BOTH || eye == XR_EYE_VISIBILITY_LEFT;

    auto& layer = is_left_eye ? this->m_slate_layer : this->m_slate_layer_right;

    layer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    const auto& ui_swapchain = vr->m_openxr->swapchains[(uint32_t)swapchain];
    layer.subImage.swapchain = ui_swapchain.handle;
    // Halo's Electra movie target contains valid RGB but does not provide a
    // compositor-safe alpha channel. Treat the eye-specific movie quad as
    // opaque; otherwise OpenXR blends it away over the black projection layer.
    layer.layerFlags = vr->is_halo_electra_cinematic_active()
        ? 0
        : XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.subImage.imageRect.offset.x = 0;
    layer.subImage.imageRect.offset.y = 0;
    layer.subImage.imageRect.extent.width = ui_swapchain.width;
    layer.subImage.imageRect.extent.height = ui_swapchain.height;
    layer.eyeVisibility = eye;

    auto glm_matrix = glm::identity<glm::mat4>();
    const auto follows_view = !force_stage_space && vr->m_overlay_component.m_ui_follows_view->value();
    const auto pose_tracking_enabled = pose_basis != nullptr || vr->is_ui_layer_pose_telemetry_enabled() || vr->is_ui_layer_pose_stabilizer_enabled();
    const auto hmd_rotation = pose_tracking_enabled ? glm::quat{vr->get_rotation(0)} : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    const auto live_pre_flattened_rotation = vr->is_decoupled_pitch_enabled() && vr->is_decoupled_pitch_ui_adjust_enabled()
        ? vr->get_pre_flattened_rotation()
        : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    const auto live_ui_rotation = make_slate_stage_rotation(vr.get(), vr->get_rotation_offset(), live_pre_flattened_rotation);
    auto applied_ui_rotation = live_ui_rotation;
    auto standing_origin = vr->get_standing_origin();
    bool stabilizer_used = false;
    const auto refusal_reason = get_ui_layer_pose_refusal_reason(pose_basis, follows_view, vr->is_ui_layer_pose_stabilizer_enabled());

    if (follows_view) {
        layer.space = vr->m_openxr->view_space;
    } else {
        if (pose_basis != nullptr && pose_basis->stabilizer_allowed) {
            applied_ui_rotation = make_slate_stage_rotation(vr.get(), pose_basis->rotation_offset, pose_basis->pre_flattened_rotation);
            standing_origin = pose_basis->standing_origin;
            stabilizer_used = true;
        }

        glm_matrix = Matrix4x4f{applied_ui_rotation};
        glm_matrix[3] += standing_origin;
        layer.space = vr->m_openxr->stage_space;
    }

    const auto size_meters = m_parent->m_slate_size->value();
    const auto meters_w = (float)ui_swapchain.width / (float)ui_swapchain.height * size_meters;
    const auto meters_h = size_meters;
    layer.size = {meters_w, meters_h};

    glm_matrix[3] -= glm_matrix[2] * m_parent->m_slate_distance->value();
    glm_matrix[3] += m_parent->m_slate_x_offset->value() * glm_matrix[0];
    glm_matrix[3] += m_parent->m_slate_y_offset->value() * glm_matrix[1];
    glm_matrix[3].w = 1.0f;

    layer.pose.orientation = runtimes::OpenXR::to_openxr(glm::quat_cast(glm_matrix));
    layer.pose.position = runtimes::OpenXR::to_openxr(glm_matrix[3]);

    vr->record_ui_layer_pose_sample(
        pose_basis,
        swapchain,
        eye,
        follows_view,
        stabilizer_used,
        hmd_rotation,
        live_ui_rotation,
        applied_ui_rotation,
        refusal_reason);

    // Check if the controller pointer intersects with the quad, and we can use this to emulate the mouse
    if (vr->is_using_controllers()) {
        // Right only for now for testing
        const auto controller_index = !vr->m_swap_controllers->value() ? vr->get_right_controller_index() : vr->get_left_controller_index();
        const auto right_controller_rot = glm::quat{vr->get_rotation(controller_index, false)};
        const auto right_controller_pos = glm::vec3{vr->get_position(controller_index, false)};

        const auto start = right_controller_pos;
        auto fwd = (right_controller_rot * glm::vec3{0.0f, 0.0f, -1.0f});
        const auto end = right_controller_pos + (fwd * 1000.0f);
        
        const auto plane_pos = glm::vec3{glm_matrix[3]};

        float intersection_distance = 0.0f;
        if (glm::intersectRayPlane<glm::vec3>(start, fwd, plane_pos, glm::normalize(glm::vec3{glm_matrix[2]}), intersection_distance)) {
            const auto intersection_point = start + (fwd * intersection_distance);

            const auto local_point = glm::inverse(glm_matrix) * glm::vec4{intersection_point, 1.0f};

            const auto w_half = meters_w / 2.0f;
            const auto h_half = meters_h / 2.0f;

            if (local_point.x >= -w_half && local_point.x <= w_half && local_point.y >= -h_half && local_point.y <= h_half) {
                const auto x = (local_point.x + w_half) / meters_w;
                const auto y = (meters_h - (local_point.y + h_half)) / meters_h;

                m_parent->m_intersect_state.quad_intersection_point = {x, y};
                m_parent->m_intersect_state.intersecting = true;

                if (auto it = vr->m_openxr->swapchains.find((uint32_t)runtimes::OpenXR::SwapchainIndex::UI); it != vr->m_openxr->swapchains.end()) {
                    const auto client_x = (int32_t)((float)it->second.width * x);
                    const auto client_y = (int32_t)((float)it->second.height * y);

                    m_parent->m_intersect_state.swapchain_intersection_point = {client_x, client_y};
                }
            } else {
                m_parent->m_intersect_state.intersecting = false;
            }
        }
    } else {
        m_parent->m_intersect_state.intersecting = false;
    }

    return layer;
}

std::optional<std::reference_wrapper<XrCompositionLayerCylinderKHR>> OverlayComponent::OpenXR::generate_slate_cylinder(
    runtimes::OpenXR::SwapchainIndex swapchain, 
    XrEyeVisibility eye,
    const UILayerPoseBasis* pose_basis,
    bool force_stage_space)
{
    auto& vr = VR::get();

    if (!vr->is_gui_enabled()) {
        return std::nullopt;
    }

    if (!vr->m_openxr->swapchains.contains((uint32_t)swapchain)) {
        return std::nullopt;
    }

    const auto is_left_eye = eye == XR_EYE_VISIBILITY_BOTH || eye == XR_EYE_VISIBILITY_LEFT;

    auto& layer = is_left_eye ? this->m_slate_layer_cylinder : this->m_slate_layer_cylinder_right;

    layer.type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR;
    const auto& ui_swapchain = vr->m_openxr->swapchains[(uint32_t)swapchain];
    layer.subImage.swapchain = ui_swapchain.handle;
    layer.layerFlags = vr->is_halo_electra_cinematic_active()
        ? 0
        : XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.subImage.imageRect.offset.x = 0;
    layer.subImage.imageRect.offset.y = 0;
    layer.subImage.imageRect.extent.width = ui_swapchain.width;
    layer.subImage.imageRect.extent.height = ui_swapchain.height;
    layer.eyeVisibility = eye;
    
    auto glm_matrix = glm::identity<glm::mat4>();
    const auto follows_view = !force_stage_space && vr->m_overlay_component.m_ui_follows_view->value();
    const auto pose_tracking_enabled = pose_basis != nullptr || vr->is_ui_layer_pose_telemetry_enabled() || vr->is_ui_layer_pose_stabilizer_enabled();
    const auto hmd_rotation = pose_tracking_enabled ? glm::quat{vr->get_rotation(0)} : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    const auto live_pre_flattened_rotation = vr->is_decoupled_pitch_enabled() && vr->is_decoupled_pitch_ui_adjust_enabled()
        ? vr->get_pre_flattened_rotation()
        : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    const auto live_ui_rotation = make_slate_stage_rotation(vr.get(), vr->get_rotation_offset(), live_pre_flattened_rotation);
    auto applied_ui_rotation = live_ui_rotation;
    auto standing_origin = vr->get_standing_origin();
    bool stabilizer_used = false;
    const auto refusal_reason = get_ui_layer_pose_refusal_reason(pose_basis, follows_view, vr->is_ui_layer_pose_stabilizer_enabled());

    if (follows_view) {
        layer.space = vr->m_openxr->view_space;
    } else {
        if (pose_basis != nullptr && pose_basis->stabilizer_allowed) {
            applied_ui_rotation = make_slate_stage_rotation(vr.get(), pose_basis->rotation_offset, pose_basis->pre_flattened_rotation);
            standing_origin = pose_basis->standing_origin;
            stabilizer_used = true;
        }

        glm_matrix = Matrix4x4f{applied_ui_rotation};
        glm_matrix[3] += standing_origin;
        layer.space = vr->m_openxr->stage_space;
    }

    const auto size_meters = m_parent->m_slate_size->value();
    const auto meters_w = (float)ui_swapchain.width / (float)ui_swapchain.height * size_meters;
    const auto meters_h = size_meters;

    // OpenXR Docs:
    // radius is the non-negative radius of the cylinder. Values of zero or floating point positive infinity are treated as an infinite cylinder.
    // centralAngle is the angle of the visible section of the cylinder, based at 0 radians, in the range of [0, 2π). It grows symmetrically around the 0 radian angle.
    // aspectRatio is the ratio of the visible cylinder section width / height. The height of the cylinder is given by: (cylinder radius × cylinder angle) / aspectRatio.
    layer.centralAngle = glm::max<float>(1.0f, glm::radians(m_parent->m_slate_cylinder_angle->value()));
    layer.aspectRatio = (meters_w / meters_h);
    layer.radius = (meters_h / layer.centralAngle) * layer.aspectRatio;

    glm_matrix[3] -= glm_matrix[2] * m_parent->m_slate_distance->value();
    glm_matrix[3] += glm_matrix[2] * layer.radius;
    glm_matrix[3] += m_parent->m_slate_x_offset->value() * glm_matrix[0];
    glm_matrix[3] += m_parent->m_slate_y_offset->value() * glm_matrix[1];
    glm_matrix[3].w = 1.0f;

    layer.pose.orientation = runtimes::OpenXR::to_openxr(glm::quat_cast(glm_matrix));
    layer.pose.position = runtimes::OpenXR::to_openxr(glm_matrix[3]);

    vr->record_ui_layer_pose_sample(
        pose_basis,
        swapchain,
        eye,
        follows_view,
        stabilizer_used,
        hmd_rotation,
        live_ui_rotation,
        applied_ui_rotation,
        refusal_reason);

    return layer;
}

std::optional<std::reference_wrapper<XrCompositionLayerBaseHeader>> OverlayComponent::OpenXR::generate_slate_layer(
    runtimes::OpenXR::SwapchainIndex swapchain, 
    XrEyeVisibility eye,
    const UILayerPoseBasis* pose_basis,
    bool force_stage_space)
{
    switch ((OverlayComponent::OverlayType)m_parent->m_slate_overlay_type->value()) {
    default:
    case OverlayComponent::OverlayType::QUAD:
        if (auto result = generate_slate_quad(swapchain, eye, pose_basis, force_stage_space); result.has_value()) {
            return *(XrCompositionLayerBaseHeader*)&result.value().get();
        }

        return std::nullopt;
    case OverlayComponent::OverlayType::CYLINDER:
        if (!VR::get()->get_runtime()->is_cylinder_layer_allowed()) {
            if (auto result = generate_slate_quad(swapchain, eye, pose_basis, force_stage_space); result.has_value()) {
                return *(XrCompositionLayerBaseHeader*)&result.value().get();
            }

            return std::nullopt;
        }

        if (auto result = generate_slate_cylinder(swapchain, eye, pose_basis, force_stage_space); result.has_value()) {
            return *(XrCompositionLayerBaseHeader*)&result.value().get();
        }

        return std::nullopt;
    };
}

std::optional<std::reference_wrapper<XrCompositionLayerBaseHeader>>
OverlayComponent::OpenXR::generate_daysgone_ahud_slate_layer() {
    auto vr = VR::get();
    if (vr == nullptr) {
        return std::nullopt;
    }

    if (!m_daysgone_ahud_pose_basis.valid) {
        m_daysgone_ahud_pose_basis.valid = true;
        m_daysgone_ahud_pose_basis.stabilizer_allowed = true;
        m_daysgone_ahud_pose_basis.rotation_offset = vr->get_rotation_offset();
        m_daysgone_ahud_pose_basis.pre_flattened_rotation =
            vr->is_decoupled_pitch_enabled() && vr->is_decoupled_pitch_ui_adjust_enabled()
            ? vr->get_pre_flattened_rotation()
            : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
        m_daysgone_ahud_pose_basis.standing_origin = vr->get_standing_origin();
        m_daysgone_ahud_pose_basis.capture_time = std::chrono::steady_clock::now();

        SPDLOG_INFO("[DaysGone][AHUD] Latched the UI layer in OpenXR stage space");
    }

    return generate_slate_layer(
        runtimes::OpenXR::SwapchainIndex::UI,
        XR_EYE_VISIBILITY_BOTH,
        &m_daysgone_ahud_pose_basis,
        true);
}

void OverlayComponent::OpenXR::reset_daysgone_ahud_pose() {
    m_daysgone_ahud_pose_basis = {};
}


std::optional<std::reference_wrapper<XrCompositionLayerQuad>> OverlayComponent::OpenXR::generate_framework_ui_quad() {
    if (!g_framework->is_drawing_anything()) {
        m_parent->m_framework_intersect_state.intersecting = false;
        return std::nullopt;
    }

    auto& vr = VR::get();

    auto& layer = this->m_framework_ui_layer;

    layer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    const auto& ui_swapchain = vr->m_openxr->swapchains[(uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI];
    layer.subImage.swapchain = ui_swapchain.handle;
    layer.subImage.imageRect.offset.x = 0;
    layer.subImage.imageRect.offset.y = 0;
    layer.subImage.imageRect.extent.width = ui_swapchain.width;
    layer.subImage.imageRect.extent.height = ui_swapchain.height;
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.eyeVisibility = XrEyeVisibility::XR_EYE_VISIBILITY_BOTH;

    auto glm_matrix = glm::identity<glm::mat4>();

    if (vr->m_overlay_component.m_framework_ui_follows_view->value()) {
        layer.space = vr->m_openxr->view_space;
    } else {
        auto rotation_offset = glm::inverse(vr->get_rotation_offset());

        // If we're not drawing the UI, this means we want to draw the cursor all the time
        // So we need to rotate the UI's pitch as well
        if (!g_framework->is_drawing_ui() && vr->is_decoupled_pitch_enabled() && vr->is_decoupled_pitch_ui_adjust_enabled()) {
            const auto pre_flat_rotation = vr->get_pre_flattened_rotation();
            const auto pre_flat_pitch = utility::math::pitch_only(pre_flat_rotation);

            // Add the inverse of the pitch rotation to the rotation offset
            rotation_offset = glm::normalize(glm::inverse(pre_flat_pitch * vr->get_rotation_offset()));
        }

        glm_matrix = Matrix4x4f{rotation_offset};
        glm_matrix[3] += vr->get_standing_origin();
        layer.space = vr->m_openxr->stage_space;
    }

    const auto size_meters = g_framework->is_drawing_ui() ? m_parent->m_framework_size->value() : m_parent->m_slate_size->value();
    const float scale_factor =  g_framework->is_drawing_ui() ? ((float)ui_swapchain.width / 1920.0f) : 1.0f;

    // Adjust size_meters based on scaling factor.
    const float adjusted_size_meters = size_meters * scale_factor;

    // Compute the new dimensions in meters.
    const auto meters_w = (float)ui_swapchain.width / (float)ui_swapchain.height * adjusted_size_meters;
    const auto meters_h = adjusted_size_meters;

    layer.size = {meters_w, meters_h};

    if (g_framework->is_drawing_ui()) {
        glm_matrix[3] -= glm_matrix[2] * m_parent->m_framework_distance->value();
    } else {
        glm_matrix[3] -= glm_matrix[2] * (m_parent->m_slate_distance->value() - 0.01f);

        glm_matrix[3] += m_parent->m_slate_x_offset->value() * glm_matrix[0];
        glm_matrix[3] += m_parent->m_slate_y_offset->value() * glm_matrix[1];
    }

    glm_matrix[3].w = 1.0f;

    layer.pose.orientation = runtimes::OpenXR::to_openxr(glm::quat_cast(glm_matrix));
    layer.pose.position = runtimes::OpenXR::to_openxr(glm_matrix[3]);

    // Check if the controller pointer intersects with the quad, and we can use this to emulate the mouse
    if (vr->is_using_controllers()) {
        // Right only for now for testing
        const auto controller_index = !vr->m_swap_controllers->value() ? vr->get_right_controller_index() : vr->get_left_controller_index();
        const auto right_controller_rot = glm::quat{vr->get_rotation(controller_index, false)};
        const auto right_controller_pos = glm::vec3{vr->get_position(controller_index, false)};

        const auto start = right_controller_pos;
        auto fwd = (right_controller_rot * glm::vec3{0.0f, 0.0f, -1.0f});
        const auto end = right_controller_pos + (fwd * 1000.0f);
        
        const auto plane_pos = glm::vec3{glm_matrix[3]};

        float intersection_distance = 0.0f;
        if (glm::intersectRayPlane<glm::vec3>(start, fwd, plane_pos, glm::normalize(glm::vec3{glm_matrix[2]}), intersection_distance)) {
            const auto intersection_point = start + (fwd * intersection_distance);

            const auto local_point = glm::inverse(glm_matrix) * glm::vec4{intersection_point, 1.0f};

            const auto w_half = meters_w / 2.0f;
            const auto h_half = meters_h / 2.0f;

            if (local_point.x >= -w_half && local_point.x <= w_half && local_point.y >= -h_half && local_point.y <= h_half) {
                const auto x = (local_point.x + w_half) / meters_w;
                const auto y = (meters_h - (local_point.y + h_half)) / meters_h;

                m_parent->m_framework_intersect_state.quad_intersection_point = {x, y};
                m_parent->m_framework_intersect_state.intersecting = true;

                if (auto it = vr->m_openxr->swapchains.find((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI); it != vr->m_openxr->swapchains.end()) {
                    const auto client_x = (int32_t)((float)it->second.width * x);
                    const auto client_y = (int32_t)((float)it->second.height * y);

                    m_parent->m_framework_intersect_state.swapchain_intersection_point = {client_x, client_y};
                }
            } else {
                m_parent->m_framework_intersect_state.intersecting = false;
            }
        }
    } else {
        m_parent->m_framework_intersect_state.intersecting = false;
    }

    return layer;
}

bool OverlayComponent::build_pot_vignette_pipeline_dx12(ID3D12Device* device) {
    if (m_pot_vignette_root_sig != nullptr && m_pot_vignette_pso != nullptr) {
        return true;
    }

    // Fullscreen-triangle falloff shader: RGB white (color modulated live via
    // SetOverlayColor), A radial smoothstep identical to the old CPU loop.
    // SolidDebug => fully opaque quad (compositor-level isolation test).
    static const char* s_vignette_vs = R"hlsl(
struct VSOut {
    float4 pos : SV_Position;
};

VSOut vs_main(uint vid : SV_VertexID) {
    VSOut o;
    o.pos = float4(vid == 1 ? 3.0f : -1.0f, vid == 2 ? 3.0f : -1.0f, 0.0f, 1.0f);
    return o;
}
)hlsl";

    static const char* s_vignette_ps = R"hlsl(
cbuffer VignetteParams : register(b0) {
    float g_start;
    float g_end;
    float g_solid;
    float g_gain;
};

struct PSOut {
    float4 color : SV_Target;
};

PSOut ps_main(float4 pos : SV_Position) {
    const float half_size = 256.0f; // 512 / 2
    const float dx = (pos.x - 0.5f - half_size) / half_size;
    const float dy = (pos.y - 0.5f - half_size) / half_size;

    float s = 1.0f;

    if (g_solid < 0.5f) {
        const float dist = sqrt(dx * dx + dy * dy) / 1.41421356f * 2.0f;
        // Gain scales the distance, not just the alpha: the whole falloff
        // ring moves inward, so the clear center hole shrinks (gain 8 leaves
        // roughly a 5% hole) instead of only saturating the outer band.
        const float t = (dist * max(g_gain, 1.0f) - g_start) / max(g_end - g_start, 0.0001f);
        const float c = saturate(t);
        s = c * c * (3.0f - 2.0f * c);
    }

    PSOut o;
    o.color = float4(1.0f, 1.0f, 1.0f, s);
    return o;
}
)hlsl";

    ComPtr<ID3DBlob> vs_blob{};
    ComPtr<ID3DBlob> ps_blob{};
    ComPtr<ID3DBlob> err_blob{};

    if (FAILED(D3DCompile(s_vignette_vs, strlen(s_vignette_vs), "pot_vignette_vs", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vs_blob, &err_blob))
        || FAILED(D3DCompile(s_vignette_ps, strlen(s_vignette_ps), "pot_vignette_ps", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &ps_blob, &err_blob))) {
        SPDLOG_ERROR("[PotVignette] shader compile failed: {}", err_blob != nullptr ? (const char*)err_blob->GetBufferPointer() : "unknown");
        return false;
    }

    D3D12_ROOT_PARAMETER root_params[1]{};
    root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_params[0].Constants.ShaderRegister = 0;
    root_params[0].Constants.RegisterSpace = 0;
    root_params[0].Constants.Num32BitValues = 4;
    root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 1;
    root_desc.pParameters = root_params;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    // Resolve the serializer from the already-loaded d3d12.dll (UEVR does not
    // statically link d3d12.lib; same approach as the imgui DX12 backend).
    using SerializeRootSignatureFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);

    const auto d3d12_module = GetModuleHandleW(L"d3d12.dll");

    if (d3d12_module == nullptr) {
        SPDLOG_ERROR("[PotVignette] d3d12.dll not loaded");
        return false;
    }

    const auto serialize = (SerializeRootSignatureFn)GetProcAddress(d3d12_module, "D3D12SerializeRootSignature");

    if (serialize == nullptr) {
        SPDLOG_ERROR("[PotVignette] D3D12SerializeRootSignature unavailable");
        return false;
    }

    ComPtr<ID3DBlob> root_blob{};

    if (FAILED(serialize(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, &err_blob))
        || FAILED(device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(), IID_PPV_ARGS(&m_pot_vignette_root_sig)))) {
        SPDLOG_ERROR("[PotVignette] root signature creation failed");
        m_pot_vignette_root_sig.Reset();
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_pot_vignette_root_sig.Get();
    pso_desc.VS = { vs_blob->GetBufferPointer(), vs_blob->GetBufferSize() };
    pso_desc.PS = { ps_blob->GetBufferPointer(), ps_blob->GetBufferSize() };

    D3D12_BLEND_DESC& blend = pso_desc.BlendState;
    blend.AlphaToCoverageEnable = FALSE;
    blend.IndependentBlendEnable = FALSE;
    for (auto& target : blend.RenderTarget) {
        target.BlendEnable = FALSE;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.SrcBlend = D3D12_BLEND_ONE;
        target.DestBlend = D3D12_BLEND_ZERO;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlendAlpha = D3D12_BLEND_ZERO;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOpEnable = FALSE;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    pso_desc.SampleMask = UINT_MAX;

    D3D12_RASTERIZER_DESC& raster = pso_desc.RasterizerState;
    raster.FillMode = D3D12_FILL_MODE_SOLID;
    raster.CullMode = D3D12_CULL_MODE_NONE;
    raster.FrontCounterClockwise = FALSE;
    raster.DepthBias = 0;
    raster.DepthBiasClamp = 0.0f;
    raster.SlopeScaledDepthBias = 0.0f;
    raster.DepthClipEnable = TRUE;
    raster.MultisampleEnable = FALSE;
    raster.AntialiasedLineEnable = FALSE;

    D3D12_DEPTH_STENCIL_DESC& depth = pso_desc.DepthStencilState;
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    depth.StencilEnable = FALSE;
    depth.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    depth.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    depth.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    depth.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    depth.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    depth.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    depth.BackFace = depth.FrontFace;

    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pso_desc.SampleDesc.Count = 1;

    if (FAILED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&m_pot_vignette_pso)))) {
        SPDLOG_ERROR("[PotVignette] pipeline state creation failed");
        m_pot_vignette_pso.Reset();
        return false;
    }

    SPDLOG_INFO("[PotVignette] GPU falloff pipeline compiled");
    return true;
}

bool OverlayComponent::ensure_pot_vignette_texture_dx12(int slot, float gain) {
    if (slot < 0 || slot >= POT_VIGNETTE_SLOT_COUNT) {
        return false;
    }

    const auto start = m_pot_vignette_start->value();
    const auto end = m_pot_vignette_end->value();
    const bool solid = m_pot_vignette_solid_debug->value();

    // Quantize gain to 0.5 steps: hole closure still reads continuous, but a
    // ramp renders a bounded ~16 passes instead of one every frame.
    gain = std::max(1.0f, std::round(gain * 2.0f) * 0.5f);

    // Shape knobs are shared by all slots; bump a version counter so every
    // slot re-renders once after a shape change.
    if (start != m_pot_vignette_last_start || end != m_pot_vignette_last_end || solid != m_pot_vignette_last_solid) {
        m_pot_vignette_last_start = start;
        m_pot_vignette_last_end = end;
        m_pot_vignette_last_solid = solid;
        ++m_pot_vignette_shape_version;
    }

    auto& front_ctx = m_pot_vignette_ctx[slot][m_pot_vignette_front[slot]];

    if (front_ctx.texture != nullptr && m_pot_vignette_slot_shape_version[slot] == m_pot_vignette_shape_version
        && gain == m_pot_vignette_last_gain[slot]) {
        return true; // idle: no CPU or GPU work at all
    }

    const auto device = g_framework->get_d3d12_hook()->get_device();
    if (device == nullptr) {
        SPDLOG_ERROR_EVERY_N_SEC(5, "[PotVignette] no D3D12 device yet");
        return false;
    }

    constexpr uint32_t SIZE = 512;

    if (!build_pot_vignette_pipeline_dx12(device)) {
        return false;
    }

    // Render into the BACK buffer and flip when complete: the compositor keeps
    // sampling the finished front texture throughout, so partial/cleared
    // content is never visible (this was the source of the per-render pulse).
    const int back = 1 - m_pot_vignette_front[slot];
    auto& ctx = m_pot_vignette_ctx[slot][back];

    if (ctx.texture == nullptr) {
        // Render-target-capable texture in the same resource style the OpenVR
        // UI texture uses for compositor submission.
        D3D12_RESOURCE_DESC tex_desc{};
        tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tex_desc.Width = SIZE;
        tex_desc.Height = SIZE;
        tex_desc.DepthOrArraySize = 1;
        tex_desc.MipLevels = 1;
        tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        tex_desc.SampleDesc.Count = 1;
        tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clear_value{};
        clear_value.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

        ComPtr<ID3D12Resource> tex{};
        D3D12_HEAP_PROPERTIES default_heap{};
        default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        if (FAILED(device->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value, IID_PPV_ARGS(&tex)))) {
            SPDLOG_ERROR("[PotVignette] failed to create vignette texture (slot {})", slot);
            return false;
        }

        if (!ctx.setup(device, tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"PotVignetteTexture")) {
            SPDLOG_ERROR("[PotVignette] failed to set up vignette texture context (slot {})", slot);
            ctx.reset();
            return false;
        }

        m_pot_vignette_in_psr[slot][back] = false;
    }

    // One persistent render context for the whole session: no per-render
    // allocator/fence churn on the present thread.
    if (!m_pot_vignette_render_ctx_ready || !m_pot_vignette_render_ctx.ready()) {
        if (!m_pot_vignette_render_ctx.setup(L"PotVignetteRender")) {
            SPDLOG_ERROR_EVERY_N_SEC(5, "[PotVignette] render CommandContext setup failed");
            m_pot_vignette_render_ctx_ready = false;
            return false;
        }

        m_pot_vignette_render_ctx_ready = m_pot_vignette_render_ctx.ready();

        if (!m_pot_vignette_render_ctx_ready) {
            SPDLOG_ERROR_EVERY_N_SEC(5, "[PotVignette] render CommandContext not ready");
            return false;
        }
    }

    auto* cmd = m_pot_vignette_render_ctx.cmd_list.Get();

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = ctx.texture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    if (m_pot_vignette_in_psr[slot][back]) {
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cmd->ResourceBarrier(1, &barrier);
    }

    const float clear_color[4]{1.0f, 1.0f, 1.0f, 0.0f};
    const auto rtv = ctx.get_rtv();
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmd->ClearRenderTargetView(rtv, clear_color, 0, nullptr);

    D3D12_VIEWPORT viewport{0.0f, 0.0f, (FLOAT)SIZE, (FLOAT)SIZE, 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, (LONG)SIZE, (LONG)SIZE};
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);

    cmd->SetGraphicsRootSignature(m_pot_vignette_root_sig.Get());
    cmd->SetPipelineState(m_pot_vignette_pso.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const float params[4]{start, end, solid ? 1.0f : 0.0f, gain};
    cmd->SetGraphicsRoot32BitConstants(0, 4, params, 0);
    cmd->DrawInstanced(3, 1, 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cmd->ResourceBarrier(1, &barrier);

    // execute() only submits when has_commands is set (raw cmd_list recording
    // does not set it; lesson from the research fork).
    m_pot_vignette_render_ctx.has_commands = true;
    m_pot_vignette_render_ctx.execute();
    m_pot_vignette_render_ctx.wait(INFINITE);

    m_pot_vignette_in_psr[slot][back] = true;
    m_pot_vignette_front[slot] = back;
    m_pot_vignette_last_gain[slot] = gain;
    m_pot_vignette_slot_shape_version[slot] = m_pot_vignette_shape_version;

    SPDLOG_INFO("[PotVignette] falloff rendered on GPU [slot {} start={} end={} solid={} gain={}]", slot, start, end, solid, gain);
    return true;
}

void OverlayComponent::update_pot_vignette_pool_openvr() {
    if (m_pot_vignette_handles[POT_VIGNETTE_SLOT_RED] == 0 || vr::VROverlay() == nullptr || vr::VRSystem() == nullptr) {
        return;
    }

    // The texture upload path is D3D12/OpenVR-only (PoT's verified config).
    if (!g_framework->is_dx12()) {
        return;
    }

    const auto hide_slot = [&](int slot) {
        if (m_pot_vignette_handles[slot] != 0) {
            vr::VROverlay()->HideOverlay(m_pot_vignette_handles[slot]);

            if (m_pot_vignette_shown[slot]) {
                m_pot_vignette_shown[slot] = false;
                spdlog::info("[PotVignette] slot {} hidden", slot);
            }
        }
    };

    if (!m_pot_vignette_enabled->value()) {
        for (auto slot = 0; slot < POT_VIGNETTE_SLOT_COUNT; ++slot) {
            hide_slot(slot);
        }
        m_pot_vignette_fail_backoff = {};
        return;
    }

    // failure backoff: retry failed slots at most every 1s while broken
    const auto now = std::chrono::steady_clock::now();
    if (m_pot_vignette_fail_backoff > now) {
        return;
    }

    // Every SetOverlay* return code is checked: a silently failing call looks
    // identical to "nothing changes".
    const auto check = [&](const char* what, vr::EVROverlayError e) {
        if (e != vr::VROverlayError_None) {
            SPDLOG_ERROR_EVERY_N_SEC(2, "[PotVignette] {} failed: {}", what,
                vr::VROverlay()->GetOverlayErrorNameFromEnum(e));
        }
    };

    // head-locked: same pose source as the slate follows-view branch
    vr::TrackedDevicePose_t pose{};
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, &pose, 1);
    const auto pose_mat = glm::rowMajor4(Matrix4x4f{*(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking});
    const auto head_rotation = glm::extractMatrixRotation(pose_mat);

    static bool first_update_logged = false;

    for (auto slot = 0; slot < POT_VIGNETTE_SLOT_COUNT; ++slot) {
        const auto handle = m_pot_vignette_handles[slot];

        if (handle == 0) {
            continue;
        }

        const auto slot_enabled = (slot == POT_VIGNETTE_SLOT_MOTION)
            ? m_pot_vignette_motion_enabled->value()
            : m_pot_vignette_damage_enabled->value();

        const auto alpha = m_pot_vignette_alphas[slot].load(std::memory_order_relaxed);

        if (!slot_enabled || alpha <= 0.002f) {
            hide_slot(slot);
            continue;
        }

        // Slot texture carries the gain (>1 intensity widens the darkened
        // region; compositor alpha itself saturates at 1).
        if (!ensure_pot_vignette_texture_dx12(slot, alpha)) {
            m_pot_vignette_fail_backoff = now + std::chrono::milliseconds(1000);
            hide_slot(slot);
            continue;
        }

        float r{};
        float g{};
        float b{};

        switch (slot) {
        case POT_VIGNETTE_SLOT_RED:
            r = m_pot_vignette_red_r->value(); g = m_pot_vignette_red_g->value(); b = m_pot_vignette_red_b->value();
            break;
        case POT_VIGNETTE_SLOT_GREEN:
            r = m_pot_vignette_green_r->value(); g = m_pot_vignette_green_g->value(); b = m_pot_vignette_green_b->value();
            break;
        case POT_VIGNETTE_SLOT_BLUE:
            r = m_pot_vignette_blue_r->value(); g = m_pot_vignette_blue_g->value(); b = m_pot_vignette_blue_b->value();
            break;
        default:
            r = m_pot_vignette_motion_r->value(); g = m_pot_vignette_motion_g->value(); b = m_pot_vignette_motion_b->value();
            break;
        }

        vr::VROverlay()->ShowOverlay(handle);

        if (!m_pot_vignette_shown[slot]) {
            m_pot_vignette_shown[slot] = true;
            spdlog::info("[PotVignette] slot {} showing (alpha={:.3f})", slot, alpha);
        }
        check("SetOverlayColor", vr::VROverlay()->SetOverlayColor(handle, r, g, b));
        check("SetOverlayAlpha", vr::VROverlay()->SetOverlayAlpha(handle, std::clamp(alpha, 0.0f, 1.0f)));
        check("SetOverlayWidthInMeters", vr::VROverlay()->SetOverlayWidthInMeters(handle, m_pot_vignette_size->value()));

        auto glm_matrix = head_rotation;
        glm_matrix[3] += pose_mat[3];
        glm_matrix[3] -= glm_matrix[2] * m_pot_vignette_distance->value();
        glm_matrix[3].w = 1.0f;

        const auto steamvr_matrix = Matrix3x4f{glm::rowMajor4(glm_matrix)};
        check("SetOverlayTransformAbsolute", vr::VROverlay()->SetOverlayTransformAbsolute(handle, vr::TrackingUniverseStanding, (vr::HmdMatrix34_t*)&steamvr_matrix));

        vr::D3D12TextureData_t texture_data{
            m_pot_vignette_ctx[slot][m_pot_vignette_front[slot]].texture.Get(),
            g_framework->get_d3d12_hook()->get_command_queue(),
            0
        };
        vr::Texture_t tex{(void*)&texture_data, vr::TextureType_DirectX12, vr::ColorSpace_Auto};
        check("SetOverlayTexture", vr::VROverlay()->SetOverlayTexture(handle, &tex));

        if (!first_update_logged) {
            first_update_logged = true;
            SPDLOG_INFO("[PotVignette] overlay update loop running (slot={} alpha={} size={}m dist={}m)",
                slot, alpha, m_pot_vignette_size->value(), m_pot_vignette_distance->value());
        }
    }
}

void OverlayComponent::draw_pot_vignette_ui() {
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNode("Procedural Vignette Overlays")) {
        m_pot_vignette_enabled->draw("Enable Vignettes");

        if (m_pot_vignette_enabled->value()) {
            m_pot_vignette_size->draw("Size (m)");
            m_pot_vignette_distance->draw("Distance (m)");
            m_pot_vignette_start->draw("Start (inner radius)");
            m_pot_vignette_end->draw("End (outer radius)");

            const auto color_row = [](const char* label, const ModSlider::Ptr& r, const ModSlider::Ptr& g, const ModSlider::Ptr& b) {
                float rgb[3] { r->value(), g->value(), b->value() };
                if (ImGui::ColorEdit3(label, rgb)) {
                    r->value() = rgb[0];
                    g->value() = rgb[1];
                    b->value() = rgb[2];
                }
            };

            ImGui::Separator();
            m_pot_vignette_damage_enabled->draw("Damage Vignettes");
            if (m_pot_vignette_damage_enabled->value()) {
                ImGui::Indent();
                color_row("Red damage", m_pot_vignette_red_r, m_pot_vignette_red_g, m_pot_vignette_red_b);
                color_row("Green damage", m_pot_vignette_green_r, m_pot_vignette_green_g, m_pot_vignette_green_b);
                color_row("Blue damage", m_pot_vignette_blue_r, m_pot_vignette_blue_g, m_pot_vignette_blue_b);
                ImGui::TextWrapped("Green/blue mirror once their widgets are discovered in the log.");
                ImGui::Unindent();
            }

            ImGui::Separator();
            m_pot_vignette_motion_enabled->draw("Camera Rotation Vignette");
            if (m_pot_vignette_motion_enabled->value()) {
                ImGui::Indent();
                color_row("Rotation color", m_pot_vignette_motion_r, m_pot_vignette_motion_g, m_pot_vignette_motion_b);
                ImGui::Unindent();
            }
        }

        m_pot_vignette_solid_debug->draw("DEBUG: solid opaque quads (isolation test)");
        ImGui::TreePop();
    }
}
}
