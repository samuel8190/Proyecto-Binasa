// =============================
// CONFIG
// =============================
const API_STATUS = "/status";
const API_HISTORY = "/history";

// =============================
// ACTUALIZAR DASHBOARD
// =============================
async function actualizarDashboard() {
    try {
        const resp = await fetch(API_STATUS);
        if (!resp.ok) throw new Error('Error en la respuesta');
        
        const data = await resp.json();
        
        console.log("Datos recibidos:", data);

        // Nivel del agua
        document.getElementById("nivelAgua").innerText = data.level + "%";

        // Estado bomba
        const pumpStateElement = document.getElementById("pumpState");
        const isPumpOn = data.pump;
        pumpStateElement.innerText = isPumpOn ? "ON" : "OFF";
        pumpStateElement.className = isPumpOn ? "value on" : "value";
        window.pumpState = isPumpOn;

        // Sensor óptico (espuma)
        document.getElementById("valorOptico").innerText = data.foam + "%";

    } catch (err) {
        console.log("Error al actualizar dashboard:", err);
        document.getElementById("nivelAgua").innerText = "?%";
        document.getElementById("pumpState").innerText = "ERROR";
        document.getElementById("valorOptico").innerText = "?%";
    }
}

// =============================
// BOTÓN "ACTUADOR" - CONTROL BOMBA
// =============================
document.getElementById("btnActuador").addEventListener("click", () => {
    const currentState = window.pumpState;
    
    Swal.fire({
        title: currentState ? "¿Apagar la bomba?" : "¿Encender la bomba?",
        text: currentState ? "La bomba se apagará." : "La bomba se encenderá.",
        icon: "question",
        showCancelButton: true,
        confirmButtonText: "Sí, " + (currentState ? "apagar" : "encender"),
        cancelButtonText: "Cancelar",
        confirmButtonColor: currentState ? "#e74c3c" : "#27ae60"
    }).then(async (result) => {
        if (result.isConfirmed) {
            try {
                const response = await fetch('/control', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json',
                    },
                    body: JSON.stringify({
                        action: currentState ? 'off' : 'on'
                    })
                });
                
                if (response.ok) {
                    Swal.fire({
                        icon: "success",
                        title: currentState ? "✅ Bomba apagada" : "✅ Bomba encendida",
                        timer: 1500,
                        showConfirmButton: false,
                    });
                    
                    setTimeout(actualizarDashboard, 2000);
                } else {
                    throw new Error('Error en el servidor');
                }
            } catch (error) {
                Swal.fire({
                    icon: "error",
                    title: "❌ Error",
                    text: "No se pudo controlar la bomba",
                    confirmButtonText: "Entendido"
                });
            }
        }
    });
});

// =============================
// GRÁFICAS EN TIEMPO REAL
// =============================
let chartInstance = null;

document.getElementById("btnGrafica").addEventListener("click", async () => {
    try {
        // Obtener fecha actual
        const today = new Date().toISOString().split('T')[0];
        
        // Cargar datos históricos
        const response = await fetch(`${API_HISTORY}?date=${today}`);
        const historicalData = await response.json();
        
        // Preparar datos para la gráfica
        const horas = historicalData.map(d => d.hora);
        const nivelesAgua = historicalData.map(d => d.agua);
        const nivelesEspuma = historicalData.map(d => d.espuma);
        
        Swal.fire({
            title: "📊 Gráfica Diaria - " + today,
            html: `
                <div style="width: 100%; max-width: 500px; margin: 0 auto;">
                    <canvas id="chartCanvas" width="400" height="300"></canvas>
                </div>
                <div style="margin-top: 15px;">
                    <label for="chartDate">📅 Seleccionar fecha: </label>
                    <input type="date" id="chartDate" value="${today}" />
                </div>
            `,
            width: 600,
            didOpen: () => {
                // Crear gráfica
                const ctx = document.getElementById('chartCanvas').getContext('2d');
                
                if (chartInstance) {
                    chartInstance.destroy();
                }
                
                chartInstance = new Chart(ctx, {
                    type: 'line',
                    data: {
                        labels: horas,
                        datasets: [
                            {
                                label: '💧 Nivel de Agua (%)',
                                data: nivelesAgua,
                                borderColor: '#3498db',
                                backgroundColor: 'rgba(52, 152, 219, 0.1)',
                                borderWidth: 2,
                                tension: 0.4,
                                fill: true
                            },
                            {
                                label: '🌿 Nivel de Espuma (%)',
                                data: nivelesEspuma,
                                borderColor: '#27ae60',
                                backgroundColor: 'rgba(39, 174, 96, 0.1)',
                                borderWidth: 2,
                                tension: 0.4,
                                fill: true
                            }
                        ]
                    },
                    options: {
                        responsive: true,
                        plugins: {
                            title: {
                                display: true,
                                text: 'Historial de Niveles'
                            }
                        },
                        scales: {
                            y: {
                                beginAtZero: true,
                                max: 100,
                                title: {
                                    display: true,
                                    text: 'Porcentaje (%)'
                                }
                            },
                            x: {
                                title: {
                                    display: true,
                                    text: 'Hora del día'
                                }
                            }
                        }
                    }
                });
                
                // Cambiar fecha
                document.getElementById('chartDate').addEventListener('change', async function() {
                    const newDate = this.value;
                    try {
                        const newResponse = await fetch(`${API_HISTORY}?date=${newDate}`);
                        const newData = await newResponse.json();
                        
                        const newHoras = newData.map(d => d.hora);
                        const newAgua = newData.map(d => d.agua);
                        const newEspuma = newData.map(d => d.espuma);
                        
                        chartInstance.data.labels = newHoras;
                        chartInstance.data.datasets[0].data = newAgua;
                        chartInstance.data.datasets[1].data = newEspuma;
                        chartInstance.update();
                        
                        Swal.getTitle().textContent = "📊 Gráfica Diaria - " + newDate;
                    } catch (error) {
                        console.error('Error al cargar datos:', error);
                    }
                });
            },
            willClose: () => {
                if (chartInstance) {
                    chartInstance.destroy();
                    chartInstance = null;
                }
            }
        });
        
    } catch (error) {
        console.error('Error al cargar gráfica:', error);
        Swal.fire({
            icon: 'error',
            title: 'Error',
            text: 'No se pudieron cargar los datos históricos'
        });
    }
});

// =============================
// INICIALIZACIÓN
// =============================
document.addEventListener('DOMContentLoaded', function() {
    console.log("BinasaMan Dashboard iniciado");
    actualizarDashboard();
});

// Actualizar cada 2 segundos
setInterval(actualizarDashboard, 2000);