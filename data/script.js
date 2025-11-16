// =============================
// CONFIG
// =============================
const SHEETS_WEBAPP_URL = "URL_DE_TU_APPS_SCRIPT_AQUI"; // reemplazar
const API_STATUS = "/status";
const API_PUMP = "/pump";

// =============================
// UTILIDADES
// =============================
function porcentajeDesdeFlotantes(f0, f1, f2) {
    const activos = f0 + f1 + f2;
    return [0, 25, 50, 75, 100][activos];
}

function adcToPercent(adc) {
    return Math.round((adc / 4095) * 100);
}

// =============================
// ACTUALIZAR DASHBOARD
// =============================
async function actualizarDashboard() {
    try {
        const resp = await fetch(API_STATUS);
        const data = await resp.json();

        // Nivel del agua
        const nivel = porcentajeDesdeFlotantes(
            data.floats[0],
            data.floats[1],
            data.floats[2]
        );
        document.getElementById("nivelAgua").innerText = nivel + "%";

        // Estado bomba
        document.getElementById("estadoBomba").innerText = data.pump ? "ON" : "OFF";
        document.getElementById("toggleBomba").checked = data.pump;

        // Sensor óptico
        const adcPercent = adcToPercent(data.adc);
        document.getElementById("valorOptico").innerText = adcPercent + "%";

    } catch (err) {
        console.log("Error al actualizar dashboard:", err);
    }
}

// =============================
// TOGGLE BOMBA
// =============================
document.getElementById("toggleBomba").addEventListener("change", async (e) => {
    const action = e.target.checked ? "on" : "off";

    await fetch(API_PUMP, {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({ action })
    });

    actualizarDashboard();
});

// =============================
// POPUP: ACTUADOR
// =============================
document.getElementById("btnActuador").addEventListener("click", () => {
    Swal.fire({
        title: "Control Manual",
        html: `
            <p>Controla la bomba manualmente desde aquí:</p>
            <label class="switch">
                <input type="checkbox" id="popupToggle">
                <span class="slider"></span>
            </label>
        `,
        didOpen: () => {
            const popupToggle = document.getElementById("popupToggle");
            popupToggle.checked = document.getElementById("toggleBomba").checked;

            popupToggle.addEventListener("change", async () => {
                const action = popupToggle.checked ? "on" : "off";

                await fetch(API_PUMP, {
                    method: "POST",
                    headers: {"Content-Type": "application/json"},
                    body: JSON.stringify({ action })
                });

                actualizarDashboard();
            });
        }
    });
});

// =============================
// POPUP: GRÁFICA
// =============================
document.getElementById("btnGrafica").addEventListener("click", () => {
    Swal.fire({
        title: "Gráfica diaria",
        html: `
            <canvas id="grafCanvas" width="300" height="200"></canvas>
            <br>
            <input type="date" id="fechaPicker" />
        `,
        didOpen: async () => {
            const fechaElem = document.getElementById("fechaPicker");

            fechaElem.valueAsDate = new Date();
            fechaElem.addEventListener("change", () => cargarGrafica(fechaElem.value));

            cargarGrafica(fechaElem.value);
        }
    });
});

// =============================
// GRAFICAR DESDE GOOGLE SHEETS
// =============================
async function cargarGrafica(fecha) {
    try {
        const resp = await fetch(`${SHEETS_WEBAPP_URL}?fecha=${fecha}`);
        const datos = await resp.json();

        const etiquetas = datos.map(d => d.hora);
        const valores = datos.map(d => d.nivel);

        const ctx = document.getElementById("grafCanvas");

        new Chart(ctx, {
            type: "line",
            data: {
                labels: etiquetas,
                datasets: [{
                    label: "Nivel del agua (%)",
                    data: valores,
                    borderWidth: 2
                }]
            }
        });

    } catch (err) {
        console.log("Error cargando gráfica:", err);
    }
}

// =============================
// LOOP
// =============================
setInterval(actualizarDashboard, 2000);
actualizarDashboard();
