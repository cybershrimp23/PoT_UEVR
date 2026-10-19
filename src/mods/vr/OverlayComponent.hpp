#pragma once

#include <string>
#include <optional>
#include <cstdint>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>

#include <d3d12.h>
#include <wrl/client.h>

#include "Mod.hpp"
#include "d3d12/TextureContext.hpp"

#include "imgui.h"

namespace vrmod{
class D3D11Component;
class D3D12Component;

struct UILayerPoseBasis {
    bool valid{false};
    bool stabilizer_allowed{false};
    uint32_t render_frame_count{};
    uint32_t openxr_internal_frame_count{};
    uint32_t openxr_internal_render_frame_count{};
    uint32_t pose_update_frame_count{};
    XrTime predicted_display_time{};
    glm::quat rotation_offset{1.0f, 0.0f, 0.0f, 0.0f};
    glm::quat pre_flattened_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vector4f standing_origin{};
    std::chrono::steady_clock::time_point capture_time{};
    std::chrono::steady_clock::time_point pose_update_time{};
};

class OverlayComponent : public ModComponent {
public:
    void on_reset();
    std::optional<std::string> on_initialize_openvr();

    void on_pre_imgui_frame() override;
    void on_post_compositor_submit();

    void on_config_save(utility::Config& cfg) override;
    void on_config_load(const utility::Config& cfg, bool set_defaults)  override;
    void on_draw_ui() override;

    auto& get_openxr() {
        return m_openxr;
    }

    struct IntersectState {
        bool intersecting{false};
        glm::vec2 quad_intersection_point{};
        glm::vec2 swapchain_intersection_point{};
    };

    const auto& get_intersect_state() const {
        return m_intersect_state;
    }

    const auto& get_framework_intersect_state() const {
        return m_intersect_state;
    }

    float get_ui_invert_alpha() const {
        return std::clamp(m_ui_invert_alpha->value(), 0.0f, 1.0f);
    }

    // Path of Titans procedural vignette overlays: one shared CPU-generated
    // radial gradient D3D12 texture, four head-locked SteamVR overlay slots.
    // Mirrors the verified implementation from the research fork.
    static constexpr int POT_VIGNETTE_SLOT_RED = 0;
    static constexpr int POT_VIGNETTE_SLOT_GREEN = 1;
    static constexpr int POT_VIGNETTE_SLOT_BLUE = 2;
    static constexpr int POT_VIGNETTE_SLOT_MOTION = 3;
    static constexpr int POT_VIGNETTE_SLOT_COUNT = 4;

    // Written from the game thread; consumed by the present thread.
    // Alpha may exceed 1.0; SteamVR decides what to do with the headroom.
    void set_pot_vignette_alpha(int slot, float alpha) {
        if (slot < 0 || slot >= POT_VIGNETTE_SLOT_COUNT) {
            return;
        }

        m_pot_vignette_alphas[slot].store(std::clamp(alpha, 0.0f, 8.0f), std::memory_order_relaxed);
    }

    bool is_pot_vignette_enabled() const {
        return m_pot_vignette_enabled->value();
    }

    // Drawn on the VR menu's Path of Titans sidebar page.
    void draw_pot_vignette_ui();

private:
    // Cached data for imgui VR overlay so we know when we need to update it
    // instead of doing it constantly every frame
    struct {
        uint32_t last_render_target_width{};
        uint32_t last_render_target_height{};
        float last_width{};
        float last_height{};
        float last_x{};
        float last_y{};
    } m_overlay_data;

    // initial input state for imgui on the left eye frame
    struct {
        ImVec2      MousePos{};
        bool        MouseDown[5]{};
        float       MouseWheel{};
        float       MouseWheelH{};
        bool        KeyCtrl{};
        bool        KeyShift{};
        bool        KeyAlt{};
        bool        KeySuper{};
        bool        KeysDown[512]{};
    } m_initial_imgui_input_state;

    // overlay handle
    vr::VROverlayHandle_t m_overlay_handle{};
    vr::VROverlayHandle_t m_thumbnail_handle{};
    vr::VROverlayHandle_t m_slate_overlay_handle{};

    vr::VROverlayHandle_t m_pot_vignette_handles[POT_VIGNETTE_SLOT_COUNT]{};

    // GPU-rendered vignette falloff: one double-buffered render-target texture
    // per slot so the compositor always samples a complete texture while the
    // back buffer is refilled; a fullscreen-triangle pixel shader redraws it
    // when the shared shape or the slot's (quantized) gain changes.
    d3d12::TextureContext m_pot_vignette_ctx[POT_VIGNETTE_SLOT_COUNT][2]{};
    int m_pot_vignette_front[POT_VIGNETTE_SLOT_COUNT]{};
    d3d12::CommandContext m_pot_vignette_render_ctx{};
    bool m_pot_vignette_render_ctx_ready{false};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_pot_vignette_root_sig{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pot_vignette_pso{};
    float m_pot_vignette_last_start{-1.0f};
    float m_pot_vignette_last_end{-1.0f};
    bool m_pot_vignette_last_solid{false};
    int32_t m_pot_vignette_shape_version{0};
    std::array<int32_t, POT_VIGNETTE_SLOT_COUNT> m_pot_vignette_slot_shape_version{{-1, -1, -1, -1}};
    float m_pot_vignette_last_gain[POT_VIGNETTE_SLOT_COUNT]{};
    bool m_pot_vignette_in_psr[POT_VIGNETTE_SLOT_COUNT][2]{};

    std::array<std::atomic<float>, POT_VIGNETTE_SLOT_COUNT> m_pot_vignette_alphas{{}};
    std::array<bool, POT_VIGNETTE_SLOT_COUNT> m_pot_vignette_shown{};
    std::chrono::steady_clock::time_point m_pot_vignette_fail_backoff{};

    bool m_closed_ui{false};
    bool m_just_closed_ui{false};
    bool m_just_opened_ui{false};
    bool m_forced_aim{false};
    
    glm::vec2 m_last_mouse_pos{};
    std::chrono::steady_clock::time_point m_last_mouse_move_time{};

    IntersectState m_intersect_state{};
    IntersectState m_framework_intersect_state{};

    enum OverlayType {
        DEFAULT = 0,
        QUAD = 0,
        CYLINDER = 1,
        MAX
    };

    static const inline std::vector<std::string> s_overlay_type_names{
        "Quad",
        "Cylinder"
    };

    const ModCombo::Ptr m_slate_overlay_type{ ModCombo::create("UI_OverlayType", s_overlay_type_names) };
    const ModSlider::Ptr m_slate_distance{ ModSlider::create("UI_Distance", 0.5f, 10.0f, 2.0f) };
    const ModSlider::Ptr m_slate_x_offset{ ModSlider::create("UI_X_Offset", -10.0f, 10.0f, 0.0f) };
    const ModSlider::Ptr m_slate_y_offset{ ModSlider::create("UI_Y_Offset", -10.0f, 10.0f, 0.0f) };
    const ModSlider::Ptr m_slate_size{ ModSlider::create("UI_Size", 0.5f, 10.0f, 2.0f) };
    const ModSlider::Ptr m_slate_cylinder_angle{ ModSlider::create("UI_Cylinder_Angle", 0.0f, 360.0f, 90.0f) };
    const ModToggle::Ptr m_ui_follows_view{ ModToggle::create("UI_FollowView", false) };
    const ModSlider::Ptr m_ui_invert_alpha{ ModSlider::create("UI_InvertAlpha", 0.0f, 1.0f, 0.01f) };

    // Path of Titans vignette overlays.
    const ModToggle::Ptr m_pot_vignette_enabled{ ModToggle::create("PotVignette_Enabled", true) };
    const ModToggle::Ptr m_pot_vignette_damage_enabled{ ModToggle::create("PotVignette_DamageEnabled", true) };
    const ModToggle::Ptr m_pot_vignette_motion_enabled{ ModToggle::create("PotVignette_MotionEnabled", true) };
    const ModSlider::Ptr m_pot_vignette_size{ ModSlider::create("PotVignette_Size", 0.5f, 8.0f, 4.0f) };
    const ModSlider::Ptr m_pot_vignette_distance{ ModSlider::create("PotVignette_Distance", 0.2f, 5.0f, 1.0f) };
    const ModSlider::Ptr m_pot_vignette_start{ ModSlider::create("PotVignette_Start", 0.05f, 1.0f, 0.55f) };
    const ModSlider::Ptr m_pot_vignette_end{ ModSlider::create("PotVignette_End", 0.1f, 1.5f, 0.95f) };
    const ModSlider::Ptr m_pot_vignette_red_r{ ModSlider::create("PotVignette_Red_R", 0.0f, 1.0f, 1.0f) };
    const ModSlider::Ptr m_pot_vignette_red_g{ ModSlider::create("PotVignette_Red_G", 0.0f, 1.0f, 0.0f) };
    const ModSlider::Ptr m_pot_vignette_red_b{ ModSlider::create("PotVignette_Red_B", 0.0f, 1.0f, 0.0f) };
    const ModSlider::Ptr m_pot_vignette_green_r{ ModSlider::create("PotVignette_Green_R", 0.0f, 1.0f, 0.0f) };
    const ModSlider::Ptr m_pot_vignette_green_g{ ModSlider::create("PotVignette_Green_G", 0.0f, 1.0f, 1.0f) };
    const ModSlider::Ptr m_pot_vignette_green_b{ ModSlider::create("PotVignette_Green_B", 0.0f, 1.0f, 0.0f) };
    const ModSlider::Ptr m_pot_vignette_blue_r{ ModSlider::create("PotVignette_Blue_R", 0.0f, 1.0f, 0.0f) };
    const ModSlider::Ptr m_pot_vignette_blue_g{ ModSlider::create("PotVignette_Blue_G", 0.0f, 1.0f, 0.35f) };
    const ModSlider::Ptr m_pot_vignette_blue_b{ ModSlider::create("PotVignette_Blue_B", 0.0f, 1.0f, 1.0f) };
    const ModSlider::Ptr m_pot_vignette_motion_r{ ModSlider::create("PotVignette_Motion_R", 0.0f, 1.0f, 0.0f) };
    const ModSlider::Ptr m_pot_vignette_motion_g{ ModSlider::create("PotVignette_Motion_G", 0.0f, 1.0f, 0.0f) };
    const ModSlider::Ptr m_pot_vignette_motion_b{ ModSlider::create("PotVignette_Motion_B", 0.0f, 1.0f, 0.0f) };
    const ModToggle::Ptr m_pot_vignette_solid_debug{ ModToggle::create("PotVignette_SolidDebug", false) };

    const ModSlider::Ptr m_framework_distance{ ModSlider::create("UI_Framework_Distance", 0.5f, 10.0f, 1.75f) };
    const ModSlider::Ptr m_framework_size{ ModSlider::create("UI_Framework_Size", 0.5f, 10.0f, 2.0f) };
    const ModToggle::Ptr m_framework_ui_follows_view{ ModToggle::create("UI_Framework_FollowView", false) };
    const ModToggle::Ptr m_framework_wrist_ui{ ModToggle::create("UI_Framework_WristUI", false) };
    const ModToggle::Ptr m_framework_mouse_emulation{ ModToggle::create("UI_Framework_MouseEmulation", true) };

public:
    OverlayComponent() 
        : m_openxr{this}
    {
        m_options = { 
            *m_slate_overlay_type,
            *m_slate_x_offset,
            *m_slate_y_offset,
            *m_slate_distance,
            *m_slate_size,
            *m_slate_cylinder_angle,
            *m_ui_follows_view,
            *m_ui_invert_alpha,
            *m_framework_distance,
            *m_framework_size,
            *m_framework_ui_follows_view,
            *m_framework_wrist_ui,
            *m_framework_mouse_emulation,
            *m_pot_vignette_enabled,
            *m_pot_vignette_damage_enabled,
            *m_pot_vignette_motion_enabled,
            *m_pot_vignette_size,
            *m_pot_vignette_distance,
            *m_pot_vignette_start,
            *m_pot_vignette_end,
            *m_pot_vignette_red_r,
            *m_pot_vignette_red_g,
            *m_pot_vignette_red_b,
            *m_pot_vignette_green_r,
            *m_pot_vignette_green_g,
            *m_pot_vignette_green_b,
            *m_pot_vignette_blue_r,
            *m_pot_vignette_blue_g,
            *m_pot_vignette_blue_b,
            *m_pot_vignette_motion_r,
            *m_pot_vignette_motion_g,
            *m_pot_vignette_motion_b,
            *m_pot_vignette_solid_debug
        };
    }

    // OpenXR
private:
    class OpenXR {
    public:
        OpenXR(OverlayComponent* parent)
            : m_parent{parent}
        {

        }

        std::optional<std::reference_wrapper<XrCompositionLayerQuad>> generate_slate_quad(
            runtimes::OpenXR::SwapchainIndex swapchain = runtimes::OpenXR::SwapchainIndex::UI, 
            XrEyeVisibility eye = XR_EYE_VISIBILITY_BOTH,
            const UILayerPoseBasis* pose_basis = nullptr,
            bool force_stage_space = false
        );
        std::optional<std::reference_wrapper<XrCompositionLayerCylinderKHR>> generate_slate_cylinder(
            runtimes::OpenXR::SwapchainIndex swapchain = runtimes::OpenXR::SwapchainIndex::UI, 
            XrEyeVisibility eye = XR_EYE_VISIBILITY_BOTH,
            const UILayerPoseBasis* pose_basis = nullptr,
            bool force_stage_space = false
        );
        std::optional<std::reference_wrapper<XrCompositionLayerBaseHeader>> generate_slate_layer(
            runtimes::OpenXR::SwapchainIndex swapchain = runtimes::OpenXR::SwapchainIndex::UI, 
            XrEyeVisibility eye = XR_EYE_VISIBILITY_BOTH,
            const UILayerPoseBasis* pose_basis = nullptr,
            bool force_stage_space = false
        );
        std::optional<std::reference_wrapper<XrCompositionLayerBaseHeader>> generate_daysgone_ahud_slate_layer();
        void reset_daysgone_ahud_pose();
        std::optional<std::reference_wrapper<XrCompositionLayerQuad>> generate_framework_ui_quad();
        
    private:
        XrCompositionLayerQuad m_slate_layer{};
        XrCompositionLayerQuad m_slate_layer_right{};
        XrCompositionLayerCylinderKHR m_slate_layer_cylinder{};
        XrCompositionLayerCylinderKHR m_slate_layer_cylinder_right{};
        UILayerPoseBasis m_daysgone_ahud_pose_basis{};
        XrCompositionLayerQuad m_framework_ui_layer{};
        OverlayComponent* m_parent{ nullptr };
        
        friend class OverlayComponent;
    } m_openxr;

private:
    void update_input_openvr();
    void update_input_mouse_emulation();
    void update_overlay_openvr();
    bool update_wrist_overlay_openvr();
    void update_slate_openvr();
    bool ensure_pot_vignette_texture_dx12(int slot, float gain);
    bool build_pot_vignette_pipeline_dx12(ID3D12Device* device);
    void update_pot_vignette_pool_openvr();
};}
