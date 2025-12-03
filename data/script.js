// =============================
// CONFIG
// =============================
const API_STATUS = "/status";
const GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbzqRpKe_OxUThm55BF1boHAyxybUYhJ7goD0jGE3nNp-P-kkCTi3i66UUBQgasqTsgB8g/exec";
const WS_URL = `ws://${window.location.hostname}:81`;

// Variables globales
let pumpState = false;
let webSocket = null;
let chartInstance = null;

// =============================
// CONEXIÓN WEBSOCKET
// =============================
function connectWebSocket() {
    webSocket = new WebSocket(WS_URL);
    
    webSocket.onopen = function() {
        console.log("WebSocket conectado");
        showNotification("Conectado al sistema", "success");
    };
    
    webSocket.onmessage = function(event) {
        try {
            const data = JSON.parse(event.data);
            console.log("Datos WebSocket:", data);
            updateDashboard(data);
        } catch (error) {
            console.error("Error procesando WebSocket:", error);
        }
    };
    
    webSocket.onclose = function() {
        console.log("WebSocket desconectado, reconectando...");
        showNotification("Desconectado, reconectando...", "warning");
        setTimeout(connectWebSocket, 3000);
    };
    
    webSocket.onerror = function(error) {
        console.error("Error WebSocket:", error);
    };
}

// =============================
// ACTUALIZAR DASHBOARD
// =============================
async function actualizarDashboard() {
    try {
        const resp = await fetch(API_STATUS);
        if (!resp.ok) throw new Error('Error en la respuesta');
        
        const data = await resp.json();
        updateDashboard(data);
        
    } catch (err) {
        console.log("Error al actualizar dashboard:", err);
        showNotification("Error de conexión", "error");
    }
}

function updateDashboard(data) {
    console.log("Actualizando dashboard con:", data);
    
    // Nivel del agua
    document.getElementById("nivelAgua").innerText = (data.level || 0) + "%";
    
    // Estado bomba
    pumpState = data.pump || false;
    const pumpStateElement = document.getElementById("pumpState");
    pumpStateElement.innerText = pumpState ? "ON" : "OFF";
    pumpStateElement.className = pumpState ? "value on" : "value";
    
    // Modo bomba
    const pumpModeElement = document.getElementById("pumpMode");
    if (pumpModeElement) {
        pumpModeElement.innerText = "Modo: " + (data.manualMode ? "MANUAL" : "AUTO");
    }
    
    // Vinaza
    const vinazaPercent = data.foam || 0;
    const vinazaElement = document.getElementById("valorVinaza");
    const vinazaStatus = document.getElementById("vinazaStatus");
    
    vinazaElement.innerText = vinazaPercent + "%";
    
    if (vinazaPercent >= 50) {
        vinazaElement.className = "value high";
        if (vinazaStatus) {
            vinazaStatus.innerText = "¡Vinaza detectada!";
            vinazaStatus.style.color = "#e74c3c";
        }
    } else {
        vinazaElement.className = "value";
        if (vinazaStatus) {
            vinazaStatus.innerText = "Sin vinaza";
            vinazaStatus.style.color = "#7f8c8d";
        }
    }
    
    // Estado del sistema
    const systemElement = document.getElementById("systemStatus");
    const sensorsCount = document.getElementById("sensorsCount");
    
    if (systemElement) {
        if (data.shutdown) {
            systemElement.innerText = "APAGADO";
            systemElement.className = "shutdown";
        } else {
            systemElement.innerText = "ACTIVO";
            systemElement.className = "";
        }
    }
    
    // Contar sensores conectados
    if (sensorsCount && data.sensorsConnected) {
        const connected = data.sensorsConnected.filter(s => s).length;
        sensorsCount.innerText = `Sensores: ${connected}/3`;
    }
    
    // Actualizar timestamp
    const now = new Date();
    const timeString = now.toLocaleTimeString('es-ES', { 
        hour: '2-digit', 
        minute: '2-digit', 
        second: '2-digit' 
    });
    document.getElementById("lastUpdate").innerText = `Última actualización: ${timeString}`;
    
    // Mostrar IP
    document.getElementById("ipAddress").innerText = `IP: ${window.location.hostname}`;
}

// =============================
// CONTROL BOMBA
// =============================
document.getElementById("btnActuador").addEventListener("click", async () => {
    const action = pumpState ? 'off' : 'on';
    const actionText = pumpState ? "apagar" : "encender";
    
    Swal.fire({
        title: `¿${pumpState ? 'Apagar' : 'Encender'} la bomba?`,
        text: `La bomba se ${actionText}á en modo manual.`,
        icon: "question",
        showCancelButton: true,
        confirmButtonText: `Sí, ${actionText}`,
        cancelButtonText: "Cancelar",
        confirmButtonColor: pumpState ? "#e74c3c" : "#27ae60"
    }).then(async (result) => {
        if (result.isConfirmed) {
            try {
                const response = await fetch('/control', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json',
                    },
                    body: JSON.stringify({
                        action: action
                    })
                });
                
                const resultData = await response.json();
                
                if (response.ok) {
                    Swal.fire({
                        icon: "success",
                        title: `✅ Bomba ${pumpState ? 'apagada' : 'encendida'}`,
                        timer: 1500,
                        showConfirmButton: false,
                    });
                    
                    setTimeout(actualizarDashboard, 1000);
                } else {
                    throw new Error(resultData.error || 'Error en el servidor');
                }
            } catch (error) {
                Swal.fire({
                    icon: "error",
                    title: "❌ Error",
                    text: error.message || "No se pudo controlar la bomba",
                    confirmButtonText: "Entendido"
                });
            }
        }
    });
});

// =============================
// GRÁFICAS DESDE GOOGLE SHEETS
// =============================
document.getElementById("btnGrafica").addEventListener("click", async () => {
    try {
        const today = new Date().toISOString().split('T')[0];
        
        Swal.fire({
            title: "📊 Cargando datos...",
            text: "Conectando a Google Sheets",
            allowOutsideClick: false,
            showConfirmButton: false,
            willOpen: () => {
                Swal.showLoading();
            }
        });
        
        // Llamar DIRECTAMENTE a Google Apps Script
        const response = await fetch(`${GOOGLE_SCRIPT_URL}?date=${today}`);
        
        if (!response.ok) {
            throw new Error('Error al conectar con Google Sheets');
        }
        
        const historicalData = await response.json();
        
        Swal.close();
        
        // Verificar si son datos de ejemplo o reales
        if (historicalData.status === "success") {
            // Si el script devuelve el formato de éxito (para test)
            Swal.fire({
                icon: 'info',
                title: 'Datos de ejemplo',
                text: 'Mostrando datos de demostración. Los datos reales se guardan en Google Sheets.',
                confirmButtonText: 'Entendido'
            }).then(() => {
                showSampleChart(today);
            });
        } else if (historicalData.length > 0) {
            // Mostrar datos del gráfico
            showChartWithData(historicalData, today);
        } else {
            // Sin datos, mostrar ejemplo
            Swal.fire({
                icon: 'info',
                title: 'Sin datos históricos',
                text: 'Mostrando datos de ejemplo. Los datos reales se están guardando en Google Sheets.',
                confirmButtonText: 'Ver ejemplo'
            }).then(() => {
                showSampleChart(today);
            });
        }
        
    } catch (error) {
        console.error('Error al cargar gráfica:', error);
        Swal.fire({
            icon: 'error',
            title: 'Error de conexión',
            text: 'No se pudieron cargar los datos de Google Sheets. Mostrando datos de ejemplo.',
            confirmButtonText: 'Ver ejemplo'
        }).then(() => {
            const today = new Date().toISOString().split('T')[0];
            showSampleChart(today);
        });
    }
});

// Función para mostrar gráfico con datos
function showChartWithData(historicalData, date) {
    // Preparar datos
    const horas = historicalData.map(d => d.hora);
    const nivelesAgua = historicalData.map(d => d.agua);
    const nivelesVinaza = historicalData.map(d => d.espuma);
    
    Swal.fire({
        title: `📊 Historial - ${date}`,
        html: `
            <div style="width: 100%; max-width: 600px; margin: 0 auto;">
                <canvas id="chartCanvas" width="500" height="350"></canvas>
            </div>
            <div style="margin-top: 15px; text-align: center;">
                <p style="color: #666; font-size: 0.9em;">
                    📈 Datos cargados desde Google Sheets
                </p>
            </div>
        `,
        width: 650,
        showConfirmButton: false,
        showCloseButton: true,
        didOpen: () => {
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
                            label: '💧 Nivel de Agua',
                            data: nivelesAgua,
                            borderColor: '#3498db',
                            backgroundColor: 'rgba(52, 152, 219, 0.1)',
                            borderWidth: 3,
                            tension: 0.3,
                            fill: true
                        },
                        {
                            label: '🌿 Vinaza Detectada',
                            data: nivelesVinaza,
                            borderColor: '#f39c12',
                            backgroundColor: 'rgba(243, 156, 18, 0.1)',
                            borderWidth: 3,
                            tension: 0.3,
                            fill: true
                        }
                    ]
                },
                options: {
                    responsive: true,
                    plugins: {
                        legend: {
                            position: 'top',
                        },
                        title: {
                            display: true,
                            text: 'Historial desde Google Sheets',
                            font: {
                                size: 16
                            }
                        }
                    },
                    scales: {
                        y: {
                            beginAtZero: true,
                            max: 100,
                            title: {
                                display: true,
                                text: 'Porcentaje (%)',
                                font: {
                                    weight: 'bold'
                                }
                            }
                        },
                        x: {
                            title: {
                                display: true,
                                text: 'Hora del día',
                                font: {
                                    weight: 'bold'
                                }
                            }
                        }
                    }
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
}

// Función para mostrar gráfico de ejemplo
function showSampleChart(date) {
    // Datos de ejemplo para demostración
    const sampleData = [
        {hora: "08:00", agua: 25, espuma: 10},
        {hora: "10:00", agua: 50, espuma: 25},
        {hora: "12:00", agua: 75, espuma: 40},
        {hora: "14:00", agua: 100, espuma: 30},
        {hora: "16:00", agua: 75, espuma: 15},
        {hora: "18:00", agua: 50, espuma: 8},
        {hora: "20:00", agua: 25, espuma: 5},
        {hora: "22:00", agua: 0, espuma: 0}
    ];
    
    showChartWithData(sampleData, date);
}

// =============================
// BOTÓN REFRESH
// =============================
document.getElementById("btnRefresh")?.addEventListener("click", () => {
    actualizarDashboard();
    showNotification("Datos actualizados", "info");
});

// =============================
// NOTIFICACIONES
// =============================
function showNotification(message, type = "info") {
    const Toast = Swal.mixin({
        toast: true,
        position: "top-end",
        showConfirmButton: false,
        timer: 3000,
        timerProgressBar: true,
        didOpen: (toast) => {
            toast.addEventListener('mouseenter', Swal.stopTimer)
            toast.addEventListener('mouseleave', Swal.resumeTimer)
        }
    });
    
    Toast.fire({
        icon: type,
        title: message
    });
}

// =============================
// INICIALIZACIÓN
// =============================
document.addEventListener('DOMContentLoaded', function() {
    console.log("BinasaMan Dashboard iniciado");
    
    // Conectar WebSocket
    connectWebSocket();
    
    // Cargar datos iniciales
    actualizarDashboard();
    
    // Actualizar cada 5 segundos
    setInterval(() => {
        if (!webSocket || webSocket.readyState !== WebSocket.OPEN) {
            actualizarDashboard();
        }
    }, 5000);
});