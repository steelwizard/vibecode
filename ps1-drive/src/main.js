import './style.css'
import * as THREE from 'three'
import { EffectComposer } from 'three/addons/postprocessing/EffectComposer.js'
import { RenderPass } from 'three/addons/postprocessing/RenderPass.js'
import { ShaderPass } from 'three/addons/postprocessing/ShaderPass.js'
import { OutputPass } from 'three/addons/postprocessing/OutputPass.js'
import { LEVELS, makeRoadTexture, rebuildMountains } from './levelConfig.js'

const W = 640
const H = 400
const ROAD_HALFW = 4.1
const PLAYER_Z = 0
const BOUNDS_X = ROAD_HALFW - 0.9

/* --- PS1 dither: ordered + 5-bit per channel (screen space) --- */
const DitherPS1Shader = {
  uniforms: {
    tDiffuse: { value: null },
    tBayer: { value: null },
  },
  vertexShader: /* glsl */ `
    varying vec2 vUv;
    void main() {
      vUv = uv;
      gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
    }
  `,
  fragmentShader: /* glsl */ `
    uniform sampler2D tDiffuse;
    uniform sampler2D tBayer;
    varying vec2 vUv;
    void main() {
      vec4 col = texture2D(tDiffuse, vUv);
      vec2 buv = mod(gl_FragCoord.xy, 4.0) / 4.0;
      float th = texture2D(tBayer, buv + 0.125).r;
      vec3 c = col.rgb;
      c = c + (th - 0.5) * 0.04;
      c = floor(c * 31.0 + 0.5) / 31.0;
      gl_FragColor = vec4(c, 1.0);
    }
  `,
}

function makeBayerDataTexture() {
  const b = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5]
  const d = new Uint8Array(16 * 4)
  for (let i = 0; i < 16; i++) {
    const v = Math.floor((b[i] / 16) * 255)
    d[i * 4] = d[i * 4 + 1] = d[i * 4 + 2] = v
    d[i * 4 + 3] = 255
  }
  const t = new THREE.DataTexture(d, 4, 4)
  t.colorSpace = THREE.NoColorSpace
  t.magFilter = THREE.NearestFilter
  t.minFilter = THREE.NearestFilter
  t.wrapS = t.wrapT = THREE.RepeatWrapping
  t.needsUpdate = true
  return t
}

function makeCar(color) {
  const g = new THREE.Group()
  const bodyM = new THREE.MeshLambertMaterial({ color, flatShading: true })
  const body = new THREE.Mesh(
    new THREE.BoxGeometry(1.75, 0.42, 3.0, 1, 1, 1),
    bodyM,
  )
  body.position.y = 0.38
  g.add(body)
  const top = new THREE.Mesh(
    new THREE.BoxGeometry(0.95, 0.3, 1.15, 1, 1, 1),
    bodyM,
  )
  top.position.set(0, 0.78, -0.12)
  g.add(top)
  const wheelM = new THREE.MeshLambertMaterial({ color: 0x0a0a0c, flatShading: true })
  const wheelGeo = new THREE.CylinderGeometry(0.2, 0.2, 0.1, 5)
  for (const [x, z] of [
    [0.75, 0.85],
    [-0.75, 0.85],
    [0.75, -0.8],
    [-0.75, -0.8],
  ]) {
    const w = new THREE.Mesh(wheelGeo, wheelM)
    w.rotation.z = Math.PI / 2
    w.position.set(x, 0.2, z)
    g.add(w)
  }
  return g
}

/* --- state --- */
const keys = new Set()
let playerX = 0
let playerSpeed = 0
let odometer = 0
let dead = false
const traffic = []
const app = document.getElementById('app')
const elSpeed = document.getElementById('hudSpeed')
const elSpeedMeter = document.getElementById('hudSpeedMeter')
const elMeters = document.getElementById('hudMeters')
const elGameover = document.getElementById('hudGameover')
const elLevelName = document.getElementById('hudLevelName')

addEventListener(
  'keydown',
  (e) => {
    keys.add(e.code)
    if (e.code === 'KeyR' || e.code === 'Enter') {
      e.preventDefault()
      reset()
    } else if (e.code === 'KeyL') {
      e.preventDefault()
      setLevel((currentLevelIndex + 1) % LEVELS.length)
    } else if (e.code === 'Digit1' || e.code === 'Numpad1') {
      e.preventDefault()
      setLevel(0)
    } else if (e.code === 'Digit2' || e.code === 'Numpad2') {
      e.preventDefault()
      setLevel(1)
    } else if (e.code === 'Digit3' || e.code === 'Numpad3') {
      e.preventDefault()
      setLevel(2)
    } else if (e.code === 'Digit4' || e.code === 'Numpad4') {
      e.preventDefault()
      setLevel(3)
    } else if (e.code === 'Digit5' || e.code === 'Numpad5') {
      e.preventDefault()
      setLevel(4)
    }
  },
  { passive: false },
)
addEventListener('keyup', (e) => keys.delete(e.code))
addEventListener('blur', () => keys.clear(), true)
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'hidden') keys.clear()
})

let currentLevelIndex = 0
let roadScroll = 0.38

function reset() {
  dead = false
  playerX = 0
  playerSpeed = 0
  odometer = 0
  elGameover.classList.add('hidden')
  traffic.forEach((t, i) => {
    t.z = t.baseZ
    t.vTraffic = 4 + (i % 5) * 2.2
    t.mesh.position.x = t.laneX
  })
  cameraTargetX = 0
}

function updateLevelPills() {
  for (const btn of document.querySelectorAll('.level-pill')) {
    const n = Number(btn.getAttribute('data-level'))
    btn.classList.toggle('is-active', n === currentLevelIndex)
  }
}

function setLevel(index) {
  if (index < 0 || index >= LEVELS.length) return
  applyLevel(index)
  reset()
}

let roadM
let amb
let sun
let grassL
let grassR
const mountainGroup = new THREE.Group()
let scene
let renderer

function applyLevel(index) {
  if (index < 0 || index >= LEVELS.length) return
  const L = LEVELS[index]
  currentLevelIndex = index
  roadScroll = L.roadScroll

  if (roadM) {
    if (roadM.map) roadM.map.dispose()
    roadM.map = makeRoadTexture(L.roadStyle)
  }

  if (scene) {
    scene.fog.color.setHex(L.fog)
    scene.fog.near = L.fogNear
    scene.fog.far = L.fogFar
    scene.background = new THREE.Color(L.bg)
  }
  if (renderer) renderer.setClearColor(L.clear, 1)

  if (amb) {
    amb.color.setHex(L.amb)
    amb.intensity = L.ambI
  }
  if (sun) {
    sun.color.setHex(L.sun)
    sun.intensity = L.sunI
    sun.position.set(L.sunPos.x, L.sunPos.y, L.sunPos.z)
  }
  if (grassL) grassL.material.color.setHex(L.grass)
  if (grassR) grassR.material.color.setHex(L.grass)
  rebuildMountains(L, mountainGroup)
  if (elLevelName) elLevelName.textContent = L.name
  updateLevelPills()
}

/* --- three.js --- */
scene = new THREE.Scene()
const L0 = LEVELS[0]
scene.fog = new THREE.Fog(L0.fog, L0.fogNear, L0.fogFar)
scene.background = new THREE.Color(L0.bg)

amb = new THREE.AmbientLight(L0.amb, L0.ambI)
scene.add(amb)
sun = new THREE.DirectionalLight(L0.sun, L0.sunI)
sun.position.set(L0.sunPos.x, L0.sunPos.y, L0.sunPos.z)
scene.add(sun)

roadM = new THREE.MeshLambertMaterial({ flatShading: true })
const road = new THREE.Mesh(
  new THREE.PlaneGeometry(ROAD_HALFW * 2, 500, 1, 1),
  roadM,
)
road.rotation.x = -Math.PI / 2
road.position.set(0, 0, -60)
road.receiveShadow = false
scene.add(road)

for (const sx of [-9, 9]) {
  const p = new THREE.Mesh(
    new THREE.PlaneGeometry(12, 500, 1, 1),
    new THREE.MeshLambertMaterial({ color: L0.grass, flatShading: true }),
  )
  p.rotation.x = -Math.PI / 2
  p.position.set(sx, -0.02, -60)
  scene.add(p)
  if (sx < 0) grassL = p
  else grassR = p
}

scene.add(mountainGroup)
rebuildMountains(L0, mountainGroup)

const player = makeCar(0xcc2a2a)
player.position.set(0, 0, PLAYER_Z)
scene.add(player)

const trafficColors = [0x2a4acc, 0xcccc2a, 0x2acc6a, 0x8a2acc, 0x2ab0cc, 0xcc6a2a]
const laneX = [-2.1, 0, 2.1]
for (let i = 0; i < 7; i++) {
  const mesh = makeCar(trafficColors[i % trafficColors.length])
  const lane = laneX[Math.floor(Math.random() * 3)]
  const z0 = -18 - i * 28 - Math.random() * 6
  mesh.position.set(lane, 0, z0)
  scene.add(mesh)
  traffic.push({
    mesh,
    laneX: lane,
    z: z0,
    baseZ: z0,
    vTraffic: 5 + (i % 4) * 1.4 + (i % 2),
  })
}

const camera = new THREE.PerspectiveCamera(58, W / H, 0.1, 300)
let cameraTargetX = 0

renderer = new THREE.WebGLRenderer({ antialias: false, powerPreference: 'high-performance' })
renderer.setSize(W, H, false)
renderer.setPixelRatio(1)
renderer.setClearColor(L0.clear, 1)
app.insertBefore(renderer.domElement, app.firstChild)

if (roadM.map) roadM.map.dispose()
roadM.map = makeRoadTexture(L0.roadStyle)
currentLevelIndex = 0
roadScroll = L0.roadScroll
if (elLevelName) elLevelName.textContent = L0.name
updateLevelPills()

const bayer = makeBayerDataTexture()
DitherPS1Shader.uniforms.tBayer.value = bayer
const ditherPass = new ShaderPass(DitherPS1Shader)

const composer = new EffectComposer(renderer)
composer.addPass(new RenderPass(scene, camera))
composer.addPass(new OutputPass())
composer.addPass(ditherPass)
composer.setSize(W, H)

function resize() {
  const scale = Math.min(window.innerWidth / W, (window.innerHeight - 2) / H)
  const cw = Math.floor(W * scale)
  const ch = Math.floor(H * scale)
  renderer.domElement.style.width = `${cw}px`
  renderer.domElement.style.height = `${ch}px`
}
resize()
addEventListener('resize', resize)

for (const btn of document.querySelectorAll('.level-pill')) {
  btn.addEventListener('click', () => setLevel(Number(btn.getAttribute('data-level'))))
}

let lastT = performance.now()
function frame(now) {
  const dt = Math.min((now - lastT) / 1000, 0.05)
  lastT = now
  if (!dead) {
    const throttle = keys.has('KeyW') || keys.has('ArrowUp')
    const brake = keys.has('KeyS') || keys.has('ArrowDown')
    const maxSp = 32
    if (brake) {
      playerSpeed = Math.max(0, playerSpeed - 26 * dt)
    } else if (throttle) {
      // Full throttle: no “rolling drag” in the same frame (avoids fighting the accelerator).
      playerSpeed = Math.min(maxSp, playerSpeed + 16 * dt)
    } else {
      playerSpeed = Math.max(0, playerSpeed - 1.1 * dt)
    }
    odometer += playerSpeed * dt * 0.3
    let steer = 0
    if (keys.has('KeyA') || keys.has('ArrowLeft')) steer -= 1
    if (keys.has('KeyD') || keys.has('ArrowRight')) steer += 1
    if (playerSpeed > 0.02) {
      const grip = 1 - Math.min(playerSpeed / 32, 1) * 0.35
      playerX += steer * 11 * dt * grip
    }
    playerX = THREE.MathUtils.clamp(playerX, -BOUNDS_X, BOUNDS_X)
  }

  player.position.x = playerX
  if (roadM.map) {
    roadM.map.offset.y = -odometer * roadScroll
  }

  for (const t of traffic) {
    if (dead) break
    t.z += (playerSpeed - t.vTraffic) * dt
    if (t.z > 6 || t.z < -140) {
      t.laneX = laneX[Math.floor(Math.random() * 3)]
      t.mesh.position.x = t.laneX
      t.z = -110 - Math.random() * 40
      t.vTraffic = 2.5 + Math.random() * 12
    }
    t.mesh.position.z = t.z
    if (t.z > -2.0 && t.z < 0.5) {
      const dx = Math.abs(playerX - t.laneX)
      if (dx < 0.88) {
        dead = true
        playerSpeed = 0
        elGameover.classList.remove('hidden')
      }
    }
  }

  cameraTargetX = THREE.MathUtils.lerp(cameraTargetX, playerX * 0.22, 0.06)
  camera.position.set(cameraTargetX, 1.9 + (playerSpeed / 32) * 0.2, 5.0 + (playerSpeed / 32) * 0.4)
  camera.lookAt(new THREE.Vector3(playerX * 0.1, 0.35, -8))

  const dispK = Math.floor(playerSpeed * 3.1)
  elSpeed.textContent = String(dispK)
  elMeters.textContent = String(Math.floor(odometer * 10))
  if (elSpeedMeter) {
    const pct = (playerSpeed / 32) * 100
    elSpeedMeter.style.width = `${Math.min(100, Math.max(0, pct))}%`
  }

  composer.render()
  requestAnimationFrame(frame)
}

requestAnimationFrame(frame)
