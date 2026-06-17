var seconds = null;
var otaTimerVar = null;
var wifiConnectInterval = null;
var currentState = null;
var selectedCurtainMode = "manual";
var selectedFanMode = "auto";

$(document).ready(function () {
    buildScheduleRows();
    bindControls();
    getUpdateStatus();
    loadState();
    setInterval(loadState, 3000);
});

function bindControls() {
    $("#curtain_opening").on("input", function () {
        $("#curtain_opening_label").text(this.value + "%");
    });
    $("#fan_manual_speed").on("input", function () {
        $("#fan_manual_speed_label").text(this.value + "%");
    });
    $("#rgb_brightness").on("input", function () {
        $("#rgb_brightness_label").text(this.value + "%");
    });

    $("#curtain_manual_mode").on("click", function () { setCurtainMode("manual"); });
    $("#curtain_schedule_mode").on("click", function () { setCurtainMode("schedule"); });
    $("#fan_auto_mode").on("click", function () { setFanMode("auto"); });
    $("#fan_manual_mode").on("click", function () { setFanMode("manual"); });

    $("#save_curtain").on("click", saveCurtain);
    $("#save_fan").on("click", saveFan);
    $("#save_rgb").on("click", saveRgb);
    $("#connect_wifi").on("click", connectWifi);
    $("#save_ap").on("click", saveApConfig);
    $("#show_sta_password").on("change", function () {
        $("#connect_pass").attr("type", this.checked ? "text" : "password");
    });
}

function apiPost(url, payload, statusId) {
    return fetch(url, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload)
    }).then(function (response) {
        if (!response.ok) {
            throw new Error("Solicitud rechazada");
        }
        return response.json();
    }).then(function () {
        if (statusId) {
            $("#" + statusId).text("Guardado");
        }
        loadState();
    }).catch(function (error) {
        if (statusId) {
            $("#" + statusId).text(error.message);
        }
    });
}

function loadState() {
    fetch("/api/state")
        .then(function (response) { return response.json(); })
        .then(function (state) {
            currentState = state;
            renderState(state);
        });
}

function renderState(state) {
    $("#temperature_reading").text(Number(state.temperature).toFixed(1) + " C");
    $("#fan_speed").text(state.fanSpeed + "%");
    $("#curtain_state").text(state.curtainOpening + "%");
    $("#alarm_state").text(state.alarmActive ? "ON" : "OFF").toggleClass("alarm", state.alarmActive);

    selectedCurtainMode = state.curtainMode;
    selectedFanMode = state.fanMode;
    setCurtainMode(selectedCurtainMode, true);
    setFanMode(selectedFanMode, true);

    $("#curtain_opening").val(state.curtainOpening);
    $("#curtain_opening_label").text(state.curtainOpening + "%");
    $("#desired_temp").val(Number(state.desiredTemp).toFixed(1));
    $("#max_temp").val(Number(state.maxTemp).toFixed(1));
    $("#fan_manual_speed").val(state.manualFanSpeed);
    $("#fan_manual_speed_label").text(state.manualFanSpeed + "%");
    $("#rgb_color").val(rgbToHex(state.rgbRed, state.rgbGreen, state.rgbBlue));
    $("#rgb_brightness").val(state.rgbBrightness);
    $("#rgb_brightness_label").text(state.rgbBrightness + "%");

    state.schedule.forEach(function (record) {
        var row = $(".schedule-row[data-record='" + record.record + "']");
        row.find(".schedule-enabled").prop("checked", record.enabled);
        row.find(".schedule-hour").val(record.hour);
        row.find(".schedule-minute").val(record.minute);
        row.find(".schedule-opening").val(record.opening);
        row.find(".schedule-opening-label").text(record.opening + "%");
    });
}

function setCurtainMode(mode, silent) {
    selectedCurtainMode = mode;
    $("#curtain_manual_mode").toggleClass("active", mode === "manual");
    $("#curtain_schedule_mode").toggleClass("active", mode === "schedule");

    if (!silent && mode === "schedule") {
        apiPost("/api/curtain", { mode: "schedule" });
    }
}

function setFanMode(mode, silent) {
    selectedFanMode = mode;
    $("#fan_auto_mode").toggleClass("active", mode === "auto");
    $("#fan_manual_mode").toggleClass("active", mode === "manual");

    if (!silent) {
        saveFan();
    }
}

function saveCurtain() {
    if (selectedCurtainMode === "schedule") {
        apiPost("/api/curtain", { mode: "schedule" });
        return;
    }

    apiPost("/api/curtain", {
        mode: "manual",
        opening: Number($("#curtain_opening").val())
    });
}

function saveFan() {
    if (selectedFanMode === "manual") {
        apiPost("/api/fan", {
            mode: "manual",
            speed: Number($("#fan_manual_speed").val()),
            desiredTemp: Number($("#desired_temp").val()),
            maxTemp: Number($("#max_temp").val())
        });
        return;
    }

    apiPost("/api/fan", {
        mode: "auto",
        desiredTemp: Number($("#desired_temp").val()),
        maxTemp: Number($("#max_temp").val())
    });
}

function saveRgb() {
    var rgb = hexToRgb($("#rgb_color").val());
    apiPost("/api/rgb", {
        red: rgb.red,
        green: rgb.green,
        blue: rgb.blue,
        brightness: Number($("#rgb_brightness").val())
    });
}

function buildScheduleRows() {
    var container = $("#schedule_rows");

    for (var i = 1; i <= 8; i++) {
        var row = $(
            "<div class='schedule-row' data-record='" + i + "'>" +
                "<label class='check'><input class='schedule-enabled' type='checkbox'><span>Reg " + i + "</span></label>" +
                "<input class='schedule-hour' type='number' min='0' max='23' value='7'>" +
                "<input class='schedule-minute' type='number' min='0' max='59' value='0'>" +
                "<input class='schedule-opening' type='range' min='0' max='100' value='50'>" +
                "<span class='schedule-opening-label'>50%</span>" +
                "<button class='btn small primary schedule-save' type='button'>Guardar</button>" +
            "</div>"
        );

        row.find(".schedule-opening").on("input", function () {
            $(this).siblings(".schedule-opening-label").text(this.value + "%");
        });

        row.find(".schedule-save").on("click", function () {
            var parent = $(this).closest(".schedule-row");
            apiPost("/api/schedule", {
                record: Number(parent.data("record")),
                enabled: parent.find(".schedule-enabled").prop("checked"),
                hour: Number(parent.find(".schedule-hour").val()),
                minute: Number(parent.find(".schedule-minute").val()),
                opening: Number(parent.find(".schedule-opening").val())
            });
        });

        container.append(row);
    }
}

function connectWifi() {
    var ssid = $("#connect_ssid").val();
    var pwd = $("#connect_pass").val();

    if (!ssid || !pwd) {
        $("#wifi_connect_status").text("SSID y password son obligatorios");
        return;
    }

    apiPost("/wifiConnect.json", {
        selectedSSID: ssid,
        pwd: pwd,
        timestamp: Date.now()
    }, "wifi_connect_status");
    startWifiConnectStatusInterval();
}

function saveApConfig() {
    var ssid = $("#ap_ssid").val();
    var pwd = $("#ap_pass").val();

    if (!ssid || (pwd.length > 0 && pwd.length < 8)) {
        $("#ap_status").text("SSID requerido; password vacio o minimo 8 caracteres");
        return;
    }

    apiPost("/apConfig.json", {
        ssid: ssid,
        password: pwd
    }, "ap_status");
}

function startWifiConnectStatusInterval() {
    stopWifiConnectStatusInterval();
    wifiConnectInterval = setInterval(getWifiConnectStatus, 2800);
}

function stopWifiConnectStatusInterval() {
    if (wifiConnectInterval != null) {
        clearInterval(wifiConnectInterval);
        wifiConnectInterval = null;
    }
}

function getWifiConnectStatus() {
    var xhr = new XMLHttpRequest();
    xhr.open("POST", "/wifiConnectStatus", false);
    xhr.send("wifi_connect_status");

    if (xhr.readyState == 4 && xhr.status == 200) {
        var response = JSON.parse(xhr.responseText);
        $("#wifi_connect_status").text("Conectando...");

        if (response.wifi_connect_status == 2) {
            $("#wifi_connect_status").text("No se pudo conectar");
            stopWifiConnectStatusInterval();
        } else if (response.wifi_connect_status == 3) {
            $("#wifi_connect_status").text("Conexion exitosa");
            stopWifiConnectStatusInterval();
        }
    }
}

function getFileInfo() {
    var input = document.getElementById("selected_file");
    var file = input.files[0];

    if (file) {
        $("#file_info").text("File: " + file.name + " | Size: " + file.size + " bytes");
    }
}

function updateFirmware() {
    var formData = new FormData();
    var fileSelect = document.getElementById("selected_file");

    if (fileSelect.files && fileSelect.files.length == 1) {
        var file = fileSelect.files[0];
        formData.set("file", file, file.name);
        $("#ota_update_status").text("Uploading " + file.name + "...");

        var request = new XMLHttpRequest();
        request.upload.addEventListener("progress", updateProgress);
        request.open("POST", "/OTAupdate");
        request.responseType = "blob";
        request.send(formData);
    } else {
        window.alert("Selecciona un archivo primero");
    }
}

function updateProgress(oEvent) {
    if (oEvent.lengthComputable) {
        getUpdateStatus();
    }
}

function getUpdateStatus() {
    var xhr = new XMLHttpRequest();
    xhr.open("POST", "/OTAstatus", false);
    xhr.send("ota_update_status");

    if (xhr.readyState == 4 && xhr.status == 200) {
        var response = JSON.parse(xhr.responseText);
        $("#latest_firmware").text(response.compile_date + " - " + response.compile_time);

        if (response.ota_update_status == 1) {
            seconds = 10;
            otaRebootTimer();
        } else if (response.ota_update_status == -1) {
            $("#ota_update_status").text("Upload error");
        }
    }
}

function otaRebootTimer() {
    $("#ota_update_status").text("OTA completo. Reiniciando en: " + seconds);

    if (--seconds == 0) {
        clearTimeout(otaTimerVar);
        window.location.reload();
    } else {
        otaTimerVar = setTimeout(otaRebootTimer, 1000);
    }
}

function hexToRgb(hex) {
    var value = hex.replace("#", "");
    return {
        red: parseInt(value.substring(0, 2), 16),
        green: parseInt(value.substring(2, 4), 16),
        blue: parseInt(value.substring(4, 6), 16)
    };
}

function rgbToHex(red, green, blue) {
    return "#" + [red, green, blue].map(function (value) {
        var hex = Number(value).toString(16);
        return hex.length == 1 ? "0" + hex : hex;
    }).join("");
}
