import * as THREE from 'three'

export const LEVELS = [
  {
    name: 'Highway',
    bg: 0x3d4a5c,
    fog: 0x4a5a6e,
    fogNear: 24,
    fogFar: 110,
    clear: 0x3d4a5c,
    roadStyle: 'highway',
    grass: 0x1a2818,
    mtn: 0x1e2a1e,
    amb: 0x606882,
    ambI: 1.0,
    sun: 0xfff0d0,
    sunI: 0.9,
    sunPos: { x: 3, y: 18, z: 7 },
    roadScroll: 0.38,
  },
  {
    name: 'Coast',
    bg: 0x4a6a78,
    fog: 0x5a7a8a,
    fogNear: 20,
    fogFar: 100,
    clear: 0x4a6a78,
    roadStyle: 'coastal',
    grass: 0x2a3a2a,
    mtn: 0x1a2a2e,
    amb: 0x7090a0,
    ambI: 0.95,
    sun: 0xffeedd,
    sunI: 0.88,
    sunPos: { x: 5, y: 16, z: 4 },
    roadScroll: 0.4,
  },
  {
    name: 'Desert',
    bg: 0x8a7048,
    fog: 0x9a8060,
    fogNear: 32,
    fogFar: 120,
    clear: 0x8a7048,
    roadStyle: 'desert',
    grass: 0x4a3c28,
    mtn: 0x4a3a2a,
    amb: 0x907860,
    ambI: 1.0,
    sun: 0xffdd90,
    sunI: 1.0,
    sunPos: { x: 2, y: 22, z: 6 },
    roadScroll: 0.35,
  },
  {
    name: 'Night',
    bg: 0x12101c,
    fog: 0x1a1830,
    fogNear: 16,
    fogFar: 70,
    clear: 0x12101c,
    roadStyle: 'night',
    grass: 0x0a0c12,
    mtn: 0x0c0e14,
    amb: 0x303060,
    ambI: 0.6,
    sun: 0xaaccff,
    sunI: 0.35,
    sunPos: { x: 8, y: 10, z: 3 },
    roadScroll: 0.36,
  },
  {
    name: 'Alpine',
    bg: 0x8898a8,
    fog: 0x98a8b8,
    fogNear: 18,
    fogFar: 90,
    clear: 0x8898a8,
    roadStyle: 'alpine',
    grass: 0x3a4848,
    mtn: 0xd8e0e8,
    amb: 0x8899aa,
    ambI: 0.9,
    sun: 0xffffff,
    sunI: 0.85,
    sunPos: { x: 1, y: 20, z: 5 },
    roadScroll: 0.4,
  },
]

function canvasTex(c, repeatX = 0.2, repeatY = 8) {
  const tex = new THREE.CanvasTexture(c)
  tex.magFilter = tex.minFilter = THREE.NearestFilter
  tex.wrapS = tex.wrapT = THREE.RepeatWrapping
  tex.repeat.set(repeatX, repeatY)
  tex.anisotropy = 0
  return tex
}

/**
 * @param {string} style
 * @returns {import('three').CanvasTexture}
 */
export function makeRoadTexture(style) {
  const c = document.createElement('canvas')
  c.width = 64
  c.height = 64
  const g = c.getContext('2d', { willReadFrequently: true })

  if (style === 'highway') {
    g.fillStyle = '#26262e'
    g.fillRect(0, 0, 64, 64)
    g.fillStyle = '#2e3038'
    for (let y = 0; y < 64; y += 4) g.fillRect(0, y, 64, 2)
    g.fillStyle = '#e8b020'
    g.fillRect(0, 0, 4, 64)
    g.fillRect(60, 0, 4, 64)
    g.fillStyle = '#dedee8'
    for (let y = 0; y < 64; y += 10) g.fillRect(30, y, 3, 5)
    return canvasTex(c, 0.2, 8)
  }

  if (style === 'coastal') {
    g.fillStyle = '#2a3438'
    g.fillRect(0, 0, 64, 64)
    g.fillStyle = '#3a4a50'
    for (let y = 0; y < 64; y += 4) g.fillRect(0, y, 64, 2)
    g.fillStyle = '#6ac0d0'
    g.fillRect(0, 0, 4, 64)
    g.fillRect(60, 0, 4, 64)
    g.fillStyle = '#e8f0f5'
    for (let y = 0; y < 64; y += 10) g.fillRect(30, y, 3, 5)
    return canvasTex(c, 0.2, 8)
  }

  if (style === 'desert') {
    g.fillStyle = '#4a3c30'
    g.fillRect(0, 0, 64, 64)
    g.fillStyle = '#5a4c40'
    for (let y = 0; y < 64; y += 4) g.fillRect(0, y, 64, 2)
    g.fillStyle = '#c47020'
    g.fillRect(0, 0, 4, 64)
    g.fillRect(60, 0, 4, 64)
    g.fillStyle = '#d8a860'
    for (let y = 0; y < 64; y += 10) g.fillRect(30, y, 3, 5)
    return canvasTex(c, 0.22, 7)
  }

  if (style === 'night') {
    g.fillStyle = '#101018'
    g.fillRect(0, 0, 64, 64)
    g.fillStyle = '#1a1a24'
    for (let y = 0; y < 64; y += 4) g.fillRect(0, y, 64, 2)
    g.fillStyle = '#2a2a3a'
    g.fillRect(0, 0, 3, 64)
    g.fillRect(61, 0, 3, 64)
    g.fillStyle = '#aaccff'
    for (let y = 0; y < 64; y += 10) g.fillRect(30, y, 2, 4)
    return canvasTex(c, 0.2, 8)
  }

  if (style === 'alpine') {
    g.fillStyle = '#8a8c94'
    g.fillRect(0, 0, 64, 64)
    g.fillStyle = '#989aa4'
    for (let y = 0; y < 64; y += 4) g.fillRect(0, y, 64, 2)
    g.fillStyle = '#6a6e7a'
    g.fillRect(0, 0, 4, 64)
    g.fillRect(60, 0, 4, 64)
    g.fillStyle = '#d0d4e0'
    for (let y = 0; y < 64; y += 10) g.fillRect(30, y, 3, 5)
    return canvasTex(c, 0.2, 8)
  }

  // fallback
  g.fillStyle = '#26262e'
  g.fillRect(0, 0, 64, 64)
  g.fillStyle = '#2e3038'
  for (let y = 0; y < 64; y += 4) g.fillRect(0, y, 64, 2)
  g.fillStyle = '#e8b020'
  g.fillRect(0, 0, 4, 64)
  g.fillRect(60, 0, 4, 64)
  g.fillStyle = '#dedee8'
  for (let y = 0; y < 64; y += 10) g.fillRect(30, y, 3, 5)
  return canvasTex(c, 0.2, 8)
}

/**
 * @param {typeof LEVELS[0]} L
 * @param {import('three').Group} group
 */
export function rebuildMountains(L, group) {
  let oldMat
  while (group.children.length) {
    const m = group.children[0]
    m.geometry?.dispose()
    if (m.material) oldMat = m.material
    group.remove(m)
  }
  oldMat?.dispose()
  const mtnMat = new THREE.MeshLambertMaterial({ color: L.mtn, flatShading: true })
  for (let i = 0; i < 12; i++) {
    const m = new THREE.Mesh(
      new THREE.ConeGeometry(4 + (i % 3), 7 + (i % 2) * 2, 4, 1, true),
      mtnMat,
    )
    m.position.set(-20 + (i / 12) * 50 + Math.sin(i) * 3, 0, -40 - (i % 4) * 8)
    m.rotation.y = 0.2 * i
    group.add(m)
  }
}
