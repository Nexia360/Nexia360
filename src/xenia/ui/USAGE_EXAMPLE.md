// Example usage of the new controller support and keyboard UI

// Enable controller navigation for ImGui
imgui_drawer->SetControllerNavigationEnabled(true);

// Show a keyboard dialog when needed
auto keyboard_dialog = KeyboardDialog::ShowKeyboard(
    imgui_drawer,
    "Enter Text",
    "",  // Initial text
    KeyboardDialog::InputType::kText,
    [](const std::string& result) {
        // Callback when user confirms input
        printf("User entered: %s\n", result.c_str());
    }
);

// Example of handling controller input events in your application
void HandleControllerInput(VirtualKey button, bool is_down) {
    if (is_down) {
        imgui_drawer->OnControllerButtonDown(button);
    } else {
        imgui_drawer->OnControllerButtonUp(button);
    }
}
