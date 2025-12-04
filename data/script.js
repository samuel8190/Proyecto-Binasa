// =============================
// CONFIG
// =============================
const API_STATUS = "/status";
const GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbzNBfhyNIXLme3hGhUazuk5Vi4YSHwf7DX8OLePAfcgTVfZIlNl4GZIvlmRvXq2QcuIiQ/exec";
const WS_URL = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.hostname + ':81';

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
// GRÁFICAS DESDE GOOGLE SHEETS - SOLO DATOS REALES
// =============================
document.getElementById("btnGrafica").addEventListener("click", async () => {
    try {
        const today = new Date().toISOString().split('T')[0];
        
        Swal.fire({
            title: "📊 Cargando datos históricos...",
            text: "Obteniendo datos reales de Google Sheets",
            allowOutsideClick: false,
            showConfirmButton: false,
            willOpen: () => {
                Swal.showLoading();
            }
        });
        
        console.log(`🔗 Solicitando datos para: ${today}`);
        console.log(`URL: ${GOOGLE_SCRIPT_URL}?date=${today}`);
        
        // Hacer la petición
        const response = await fetch(`${GOOGLE_SCRIPT_URL}?date=${today}`);
        
        if (!response.ok) {
            throw new Error(`Error HTTP: ${response.status}`);
        }
        
        const historicalData = await response.json();
        console.log("📊 Datos recibidos:", historicalData);
        
        // Verificar si es un array
        if (!Array.isArray(historicalData)) {
            if (historicalData && historicalData.status === "error") {
                throw new Error(historicalData.message || "Error en el servidor");
            }
            throw new Error("Formato de datos incorrecto recibido del servidor");
        }
        
        Swal.close();
        
        // Si no hay datos
        if (historicalData.length === 0) {
            Swal.fire({
                icon: 'info',
                title: 'Sin datos históricos',
                text: `No hay registros para la fecha ${today} en Google Sheets.`,
                footer: '<small>Los datos se guardan automáticamente cada minuto.</small>',
                confirmButtonText: 'Aceptar'
            });
            return;
        }
        
        // Mostrar gráfico con datos reales
        showChartWithData(historicalData, today);
        
    } catch (error) {
        console.error('❌ Error al cargar gráfica:', error);
        Swal.fire({
            icon: 'error',
            title: 'Error de conexión',
            text: 'No se pudieron cargar los datos históricos de Google Sheets.',
            footer: `<small>Detalle: ${error.message}</small>`,
            confirmButtonText: 'Aceptar'
        });
    }
});

// Función para mostrar gráfico con datos REALES
function showChartWithData(historicalData, date) {
    // Preparar datos
    const horas = historicalData.map(d => d.hora);
    const nivelesAgua = historicalData.map(d => d.agua);
    const nivelesVinaza = historicalData.map(d => d.espuma);
    
    // Estadísticas
    const maxAgua = Math.max(...nivelesAgua);
    const minAgua = Math.min(...nivelesAgua);
    const avgAgua = (nivelesAgua.reduce((a, b) => a + b, 0) / nivelesAgua.length).toFixed(1);
    
    const maxVinaza = Math.max(...nivelesVinaza);
    const minVinaza = Math.min(...nivelesVinaza);
    const avgVinaza = (nivelesVinaza.reduce((a, b) => a + b, 0) / nivelesVinaza.length).toFixed(1);
    
    Swal.fire({
        title: `📊 Historial - ${date}`,
        html: `
            <div style="width: 100%; max-width: 700px; margin: 0 auto;">
                <canvas id="chartCanvas" width="600" height="350"></canvas>
            </div>
            <div style="margin-top: 15px; text-align: center; font-size: 0.9em;">
                <div style="display: flex; justify-content: space-around; margin-bottom: 10px;">
                    <div style="color: #3498db;">
                        <strong>💧 Agua:</strong><br>
                        Máx: ${maxAgua}% | Mín: ${minAgua}% | Prom: ${avgAgua}%
                    </div>
                    <div style="color: #f39c12;">
                        <strong>🌿 Vinaza:</strong><br>
                        Máx: ${maxVinaza}% | Mín: ${minVinaza}% | Prom: ${avgVinaza}%
                    </div>
                </div>
                <p style="color: #666;">
                    📈 ${historicalData.length} registros reales cargados desde Google Sheets
                </p>
                <p style="color: #27ae60; font-size: 0.8em;">
                    Último registro: ${horas[horas.length-1]} - Agua: ${nivelesAgua[nivelesAgua.length-1]}% | Vinaza: ${nivelesVinaza[nivelesVinaza.length-1]}%
                </p>
            </div>
        `,
        width: 750,
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
                            label: '💧 Nivel de Agua (%)',
                            data: nivelesAgua,
                            borderColor: '#3498db',
                            backgroundColor: 'rgba(52, 152, 219, 0.1)',
                            borderWidth: 3,
                            tension: 0.2,
                            fill: true,
                            pointRadius: 3,
                            pointHoverRadius: 6,
                            pointBackgroundColor: '#3498db'
                        },
                        {
                            label: '🌿 Vinaza Detectada (%)',
                            data: nivelesVinaza,
                            borderColor: '#f39c12',
                            backgroundColor: 'rgba(243, 156, 18, 0.1)',
                            borderWidth: 3,
                            tension: 0.2,
                            fill: true,
                            pointRadius: 3,
                            pointHoverRadius: 6,
                            pointBackgroundColor: '#f39c12'
                        }
                    ]
                },
                options: {
                    responsive: true,
                    maintainAspectRatio: false,
                    plugins: {
                        legend: {
                            position: 'top',
                            labels: {
                                font: {
                                    size: 12,
                                    weight: 'bold'
                                },
                                padding: 20
                            }
                        },
                        title: {
                            display: true,
                            text: 'Historial de Datos - BinasaMan',
                            font: {
                                size: 16,
                                weight: 'bold'
                            },
                            padding: {
                                top: 10,
                                bottom: 30
                            }
                        },
                        tooltip: {
                            mode: 'index',
                            intersect: false,
                            backgroundColor: 'rgba(0, 0, 0, 0.8)',
                            titleFont: {
                                size: 13
                            },
                            bodyFont: {
                                size: 13
                            },
                            padding: 10,
                            callbacks: {
                                label: function(context) {
                                    let label = context.dataset.label || '';
                                    if (label) {
                                        label = label.split(' (')[0] + ': ';
                                    }
                                    label += context.parsed.y + '%';
                                    return label;
                                }
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
                                    size: 13,
                                    weight: 'bold'
                                }
                            },
                            grid: {
                                color: 'rgba(0,0,0,0.05)'
                            },
                            ticks: {
                                font: {
                                    size: 11
                                }
                            }
                        },
                        x: {
                            title: {
                                display: true,
                                text: 'Hora del día',
                                font: {
                                    size: 13,
                                    weight: 'bold'
                                }
                            },
                            grid: {
                                color: 'rgba(0,0,0,0.05)'
                            },
                            ticks: {
                                font: {
                                    size: 11
                                },
                                maxRotation: 45,
                                minRotation: 45
                            }
                        }
                    },
                    interaction: {
                        intersect: false,
                        mode: 'index'
                    },
                    animation: {
                        duration: 1000
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