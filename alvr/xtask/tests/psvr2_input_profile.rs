// MIT: contract between the packaged SteamVR resources and ALVR's input registration.
#[allow(dead_code)]
#[path = "../src/build.rs"]
mod build;
#[allow(dead_code)]
#[path = "../src/command.rs"]
mod command;

use alvr_filesystem::{self as afs, Layout};
use serde_json::{json, Value};
use std::{collections::BTreeMap, fs, path::Path};
use xshell::Shell;

fn read(path: impl AsRef<Path>) -> String {
    fs::read_to_string(path.as_ref()).unwrap_or_else(|e| panic!("{}: {e}", path.as_ref().display()))
}

fn quoted(text: &str) -> Vec<&str> {
    text.split('"').skip(1).step_by(2).collect()
}

fn after<'a>(text: &'a str, marker: &str) -> &'a str {
    text.split_once(marker).unwrap().1
}

fn load(path: impl AsRef<Path>) -> Value {
    serde_json::from_str(&read(path)).unwrap()
}

#[test]
fn packaged_psvr2_contract() {
    let sh = Shell::new().unwrap();
    let root = afs::workspace_dir();
    let scratch = root.join(format!(
        "local-scratch/psvr2-package-{}",
        std::process::id()
    ));
    sh.create_dir(&scratch).unwrap();
    let layout = Layout::new(&scratch);
    sh.create_dir(&layout.openvr_driver_root_dir).unwrap();
    build::copy_streamer_resources(&sh, &layout);
    let props = read(root.join("alvr/server_openvr/src/props.rs"));
    let props = after(&props, "ControllersEmulationMode::PSVR2Sense => {");
    let profile_path = quoted(props)
        .into_iter()
        .find(|s| s.ends_with("_profile.json"))
        .unwrap();
    let profile_path = layout
        .resources_dir()
        .join(profile_path.strip_prefix("{alvr_server}/").unwrap());
    let profile = load(&profile_path);
    let controller_type = quoted(after(props, "set_prop(ControllerTypeString,"))[0];
    assert_eq!(profile["controller_type"], controller_type);
    assert_eq!(profile["jsonid"], "input_profile");
    assert_eq!(
        profile["compatibility_mode_controller_type"],
        "oculus_touch"
    );
    assert_eq!(profile["device_class"], "TrackedDeviceClass_Controller");
    assert_eq!(profile["input_bindingui_mode"], "controller_handed");
    let resolve = |name: &str| {
        if let Some(relative) = name.strip_prefix("{alvr_server}/") {
            layout.resources_dir().join(relative)
        } else {
            profile_path.parent().unwrap().join(name)
        }
    };
    if let Some(legacy) = profile["legacy_binding"].as_str() {
        assert!(resolve(legacy).is_file());
    }
    let remapping = load(resolve(profile["remapping"].as_str().unwrap()));
    assert_eq!(remapping["to_controller_type"], controller_type);
    assert_eq!(load(layout.openvr_driver_manifest())["name"], "alvr_server");

    // Derive registrations from the selected Rust button set and the C++ aliases/types.
    let common = read(root.join("alvr/common/src/inputs.rs"));
    let selected = after(&common, "*PSVR2_CONTROLLER_PROFILE_ID,");
    let selected = after(selected, "button_set: [").split(']').next().unwrap();
    let paths = read(root.join("alvr/server_openvr/cpp/alvr_server/Paths.cpp"));
    let controller = read(root.join("alvr/server_openvr/cpp/alvr_server/Controller.cpp"));
    for hand in ["left", "right"] {
        let mut expected = BTreeMap::new();
        for id in selected
            .split('*')
            .skip(1)
            .map(|s| s.split("_ID").next().unwrap())
        {
            let (side, name) = id.split_once('_').unwrap();
            if side.to_lowercase() != hand {
                continue;
            }
            let definition = common
                .lines()
                .find(|l| l.trim_start().starts_with(&format!("({name},")))
                .unwrap();
            let input = format!("/user/hand/{hand}/input/{}", quoted(definition)[0]);
            let entry = paths
                .split("_CONTROLLER_BUTTON_MAPPING.insert")
                .skip(1)
                .find(|entry| quoted(entry)[0] == input)
                .unwrap();
            let (aliases, kind) = entry.split_once("ButtonType::").unwrap();
            let kind = if kind.starts_with("Binary") {
                "bool"
            } else if kind.starts_with("ScalarOneSided") {
                "scalar"
            } else {
                "axis"
            };
            for alias in quoted(aliases).into_iter().skip(1) {
                expected.insert(alias.to_owned(), kind);
            }
        }
        for creation in controller.split("vr_driver_input->Create").skip(1) {
            let path = quoted(creation)[0];
            let kind = if creation.starts_with("Scalar") {
                "scalar"
            } else if creation.starts_with("Haptic") {
                "vibration"
            } else {
                "skeleton"
            };
            if kind != "skeleton" || path.ends_with(hand) {
                expected.insert(path.to_owned(), kind);
            }
        }
        expected.insert("/pose/raw".into(), "pose");
        let mut declared = BTreeMap::new();
        for (path, source) in profile["input_source"].as_object().unwrap() {
            if source["side"].as_str().is_some_and(|side| side != hand) {
                continue;
            }
            let kind = source["type"].as_str().unwrap();
            if path.starts_with("/input/finger/") {
                assert_eq!(kind, "trigger");
                declared.insert(path.clone(), "scalar");
                continue;
            }
            match kind {
                "button" | "trigger" | "joystick" => {
                    if kind == "joystick" {
                        for axis in ["x", "y"] {
                            declared.insert(format!("{path}/{axis}"), "axis");
                        }
                    }
                    for (capability, component_kind) in [
                        ("click", "bool"),
                        ("touch", "bool"),
                        ("value", "scalar"),
                        ("force", "scalar"),
                    ] {
                        if source[capability] == true {
                            assert!(kind == "trigger" || capability != "value");
                            declared.insert(format!("{path}/{capability}"), component_kind);
                        }
                    }
                }
                "pose" | "skeleton" | "vibration" => {
                    declared.insert(path.clone(), kind);
                }
                _ => panic!("unsupported source type {kind}"),
            }
        }
        assert_eq!(
            declared, expected,
            "{hand}: profile must match created components in both directions"
        );
    }
    for (from, priority, stick, left_a, left_b) in [
        ("oculus_touch", 100, "joystick", "x", "y"),
        ("knuckles", 90, "thumbstick", "a", "b"),
    ] {
        let layout = remapping["layouts"]
            .as_array()
            .unwrap()
            .iter()
            .find(|l| l["from_controller_type"] == from)
            .unwrap();
        assert_eq!(layout["priority"], priority);
        for flag in [
            "simulate_controller_type",
            "simulate_render_model",
            "simulate_HMD",
        ] {
            assert_eq!(layout[flag], true);
        }
        #[rustfmt::skip]
        let pairs = [
            ("left", [("trigger", "l2"), ("grip", "l1"), (stick, "left_stick"), (left_a, "square"), (left_b, "triangle")]),
            ("right", [("trigger", "r2"), ("grip", "r1"), (stick, "right_stick"), ("a", "cross"), ("b", "circle")]),
        ];
        for (hand, pairs) in pairs {
            for (old, new) in pairs {
                assert!(layout["autoremappings"].as_array().unwrap().contains(&json!({"from": format!("/user/hand/{hand}/input/{old}"), "to": format!("/user/hand/{hand}/input/{new}"), "mirror": false})));
            }
        }
        if from == "oculus_touch" {
            assert!(layout["remappings"].as_array().unwrap().contains(&json!({
                "from": {"path": "/user/hand/left/input/application_menu", "mode": "button", "input": "click"},
                "to": {"path": "/user/hand/right/input/options", "mode": "complex_button", "input": "single"},
                "mirror": false
            })));
        }
    }
    sh.remove_path(&scratch).unwrap();
}
