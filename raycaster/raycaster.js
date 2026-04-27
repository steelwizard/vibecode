const canvas = document.getElementById("view");
const ctx = canvas.getContext("2d");

const LEVELS = [
  {
    playerStart: { x: 1.5, y: 1.5, angle: 0 },
    exitDoor: { x: 11.5, y: 4.5 },
    map: [
      [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1],
      [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1],
      [1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1],
      [1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 1],
      [1, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 5],
      [1, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 1],
      [1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0, 1],
      [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1],
      [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1]
    ],
    enemies: [
      { x: 4.5, y: 3.5 },
      { x: 8.5, y: 2.5 },
      { x: 9.5, y: 6.5 },
      { x: 3.5, y: 7.5 }
    ],
    healthItems: [
      { x: 6.5, y: 1.5 },
      { x: 10.5, y: 5.5 }
    ]
  },
  {
    playerStart: { x: 1.5, y: 6.5, angle: -Math.PI / 7 },
    exitDoor: { x: 6.5, y: 0.5 },
    map: [
      [2, 2, 2, 2, 2, 2, 5, 2, 2, 2, 2, 2],
      [2, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 2],
      [2, 0, 3, 3, 0, 0, 3, 0, 4, 4, 0, 2],
      [2, 0, 0, 3, 0, 0, 0, 0, 4, 0, 0, 2],
      [2, 0, 0, 3, 0, 4, 4, 0, 4, 0, 0, 2],
      [2, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 2],
      [2, 0, 4, 4, 0, 0, 3, 0, 0, 0, 0, 2],
      [2, 0, 0, 0, 0, 0, 3, 0, 4, 4, 0, 2],
      [2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2]
    ],
    enemies: [
      { x: 5.5, y: 2.5 },
      { x: 9.5, y: 1.5 },
      { x: 8.5, y: 5.5 },
      { x: 3.5, y: 6.5 },
      { x: 9.5, y: 7.5 }
    ],
    healthItems: [
      { x: 2.5, y: 3.5 },
      { x: 6.5, y: 6.5 }
    ]
  },
  {
    playerStart: { x: 1.5, y: 1.5, angle: Math.PI / 6 },
    exitDoor: { x: 11.5, y: 7.5 },
    map: [
      [4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4],
      [4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4],
      [4, 0, 2, 2, 2, 0, 3, 3, 3, 0, 0, 4],
      [4, 0, 2, 0, 2, 0, 3, 0, 3, 0, 0, 4],
      [4, 0, 2, 0, 2, 0, 3, 0, 3, 0, 0, 4],
      [4, 0, 2, 0, 2, 0, 3, 0, 3, 0, 0, 4],
      [4, 0, 2, 2, 2, 0, 3, 3, 3, 0, 0, 4],
      [4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5],
      [4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4]
    ],
    enemies: [
      { x: 5.5, y: 1.5 },
      { x: 8.5, y: 2.5 },
      { x: 2.5, y: 4.5 },
      { x: 8.5, y: 4.5 },
      { x: 2.5, y: 6.5 },
      { x: 9.5, y: 6.5 }
    ],
    healthItems: [
      { x: 1.5, y: 7.5 },
      { x: 6.5, y: 4.5 },
      { x: 10.5, y: 3.5 }
    ]
  }
];

let currentLevelIndex = 0;
let map = [];
let mapW = 0;
let mapH = 0;
const FOV = Math.PI / 3;
const MAX_DIST = 24;
const MOVE_SPEED = 3;
const TURN_SPEED = 2.2;
const PLAYER_RADIUS = 0.2;
const SHOT_COOLDOWN = 0.2;
const ENEMY_RADIUS = 0.28;
const ENEMY_HEALTH = 3;
const ENEMY_SPEED = 1.15;
const ENEMY_SHOOT_RANGE = 6.5;
const ENEMY_STOP_RANGE = 1.8;
const ENEMY_FIRE_COOLDOWN = 0.95;
const PLAYER_HEALTH_MAX = 100;
const PLAYER_HIT_DAMAGE = 12;
const HEALTH_ITEM_AMOUNT = 30;
const HEALTH_ITEM_PICKUP_DIST = 0.55;

const player = {
  x: 1.5,
  y: 1.5,
  angle: 0,
  health: PLAYER_HEALTH_MAX,
  hurtTimer: 0,
  dead: false
};

const keys = new Set();
let lastTime = performance.now();
let shootCooldown = 0;
let muzzleFlashTimer = 0;
let hitFlashTimer = 0;
let gameWon = false;
let enemies = [];
let healthItems = [];
let exitDoor = null;
let exitDoorOpen = false;

window.addEventListener("keydown", (event) => {
  keys.add(event.key.toLowerCase());
  if (event.code === "Space") {
    tryShoot();
  }
  if (event.key.toLowerCase() === "r" && (player.dead || gameWon)) {
    restartGame();
  }
});

window.addEventListener("keyup", (event) => {
  keys.delete(event.key.toLowerCase());
});

window.addEventListener("mousedown", () => {
  tryShoot();
});

function isWall(x, y) {
  const mx = Math.floor(x);
  const my = Math.floor(y);
  return isBlockingTileAt(mx, my);
}

function isBlockingTileAt(mx, my) {
  if (mx < 0 || my < 0 || mx >= mapW || my >= mapH) {
    return true;
  }
  const tile = map[my][mx];
  if (tile === 0) {
    return false;
  }
  if (tile === 5) {
    return !exitDoorOpen;
  }
  return true;
}

function canOccupy(x, y) {
  return (
    !isWall(x - PLAYER_RADIUS, y - PLAYER_RADIUS) &&
    !isWall(x + PLAYER_RADIUS, y - PLAYER_RADIUS) &&
    !isWall(x - PLAYER_RADIUS, y + PLAYER_RADIUS) &&
    !isWall(x + PLAYER_RADIUS, y + PLAYER_RADIUS)
  );
}

function canEnemyOccupy(x, y) {
  return (
    !isWall(x - ENEMY_RADIUS, y - ENEMY_RADIUS) &&
    !isWall(x + ENEMY_RADIUS, y - ENEMY_RADIUS) &&
    !isWall(x - ENEMY_RADIUS, y + ENEMY_RADIUS) &&
    !isWall(x + ENEMY_RADIUS, y + ENEMY_RADIUS)
  );
}

function placePlayerInFreeCell() {
  if (canOccupy(player.x, player.y)) {
    return;
  }

  for (let y = 1; y < mapH - 1; y += 1) {
    for (let x = 1; x < mapW - 1; x += 1) {
      if (canOccupy(x + 0.5, y + 0.5)) {
        player.x = x + 0.5;
        player.y = y + 0.5;
        return;
      }
    }
  }
}

function findNearestValidSpawn(startX, startY, validator) {
  if (validator(startX, startY)) {
    return { x: startX, y: startY };
  }

  let best = null;
  for (let y = 1; y < mapH - 1; y += 1) {
    for (let x = 1; x < mapW - 1; x += 1) {
      const cx = x + 0.5;
      const cy = y + 0.5;
      if (!validator(cx, cy)) {
        continue;
      }
      const dist = Math.hypot(cx - startX, cy - startY);
      if (!best || dist < best.dist) {
        best = { x: cx, y: cy, dist };
      }
    }
  }
  return best ? { x: best.x, y: best.y } : null;
}

function movePlayer(nextX, nextY) {
  // Resolve each axis separately to keep simple sliding behavior.
  if (canOccupy(nextX, player.y)) {
    player.x = nextX;
  }
  if (canOccupy(player.x, nextY)) {
    player.y = nextY;
  }
}

function update(dt) {
  shootCooldown = Math.max(0, shootCooldown - dt);
  muzzleFlashTimer = Math.max(0, muzzleFlashTimer - dt);
  hitFlashTimer = Math.max(0, hitFlashTimer - dt);
  player.hurtTimer = Math.max(0, player.hurtTimer - dt);
  for (const enemy of enemies) {
    enemy.hitTimer = Math.max(0, enemy.hitTimer - dt);
    enemy.fireCooldown = Math.max(0, enemy.fireCooldown - dt);
  }

  if (player.dead || gameWon) {
    return;
  }

  if (keys.has("arrowleft") || keys.has("q")) {
    player.angle -= TURN_SPEED * dt;
  }
  if (keys.has("arrowright") || keys.has("e")) {
    player.angle += TURN_SPEED * dt;
  }

  const forward =
    (keys.has("arrowup") || keys.has("w") ? 1 : 0) -
    (keys.has("arrowdown") || keys.has("s") ? 1 : 0);
  const strafe = (keys.has("d") ? 1 : 0) - (keys.has("a") ? 1 : 0);

  if (forward !== 0 || strafe !== 0) {
    const vx = Math.cos(player.angle);
    const vy = Math.sin(player.angle);
    const sx = Math.cos(player.angle + Math.PI / 2);
    const sy = Math.sin(player.angle + Math.PI / 2);

    const magnitude = Math.hypot(forward, strafe);
    const inputForward = forward / magnitude;
    const inputStrafe = strafe / magnitude;

    const speed = MOVE_SPEED * dt;
    const nextX = player.x + (vx * inputForward + sx * inputStrafe) * speed;
    const nextY = player.y + (vy * inputForward + sy * inputStrafe) * speed;
    movePlayer(nextX, nextY);
  }

  updateEnemies(dt);
  collectHealthItems();

  if (!exitDoorOpen && !enemies.some((enemy) => enemy.alive)) {
    exitDoorOpen = true;
  }

  if (exitDoorOpen && canUseExitDoor()) {
      advanceToNextLevel();
  }
}

function canUseExitDoor() {
  if (!exitDoor) {
    return false;
  }
  const doorMX = Math.floor(exitDoor.x);
  const doorMY = Math.floor(exitDoor.y);
  const playerMX = Math.floor(player.x);
  const playerMY = Math.floor(player.y);
  const manhattan = Math.abs(playerMX - doorMX) + Math.abs(playerMY - doorMY);
  if (manhattan !== 1) {
    return false;
  }
  const distToDoor = Math.hypot(player.x - exitDoor.x, player.y - exitDoor.y);
  return distToDoor <= 1.25;
}

function doorStatusText() {
  if (!exitDoor) {
    return "";
  }
  return exitDoorOpen ? "Exit: Open" : "Exit: Locked";
}

function getTileAt(mx, my) {
  if (mx < 0 || my < 0 || mx >= mapW || my >= mapH) {
    return 1;
  }
  return map[my][mx];
}

function isDoorCell(mx, my) {
  return getTileAt(mx, my) === 5;
}

function isItemSpawnOpen(x, y) {
  const mx = Math.floor(x);
  const my = Math.floor(y);
  return !isBlockingTileAt(mx, my) && !isDoorCell(mx, my);
}

function isEnemySpawnOpen(x, y) {
  const mx = Math.floor(x);
  const my = Math.floor(y);
  if (isDoorCell(mx, my)) {
    return false;
  }
  return canEnemyOccupy(x, y);
}

function findBorderDoorCell() {
  for (let y = 0; y < mapH; y += 1) {
    for (let x = 0; x < mapW; x += 1) {
      const isBorder = x === 0 || y === 0 || x === mapW - 1 || y === mapH - 1;
      if (isBorder && map[y][x] === 5) {
        return { x: x + 0.5, y: y + 0.5 };
      }
    }
  }
  return null;
}

function findFallbackBorderDoorCell() {
  const candidates = [];
  for (let y = 0; y < mapH; y += 1) {
    for (let x = 0; x < mapW; x += 1) {
      const isBorder = x === 0 || y === 0 || x === mapW - 1 || y === mapH - 1;
      if (!isBorder || map[y][x] !== 0) {
        continue;
      }
      let hasInnerNeighbor = false;
      for (let dy = -1; dy <= 1; dy += 1) {
        for (let dx = -1; dx <= 1; dx += 1) {
          if (Math.abs(dx) + Math.abs(dy) !== 1) {
            continue;
          }
          const nx = x + dx;
          const ny = y + dy;
          if (nx > 0 && ny > 0 && nx < mapW - 1 && ny < mapH - 1 && map[ny][nx] === 0) {
            hasInnerNeighbor = true;
          }
        }
      }
      if (hasInnerNeighbor) {
        candidates.push({ x, y });
      }
    }
  }

  if (candidates.length === 0) {
    return null;
  }

  const midX = mapW / 2;
  const midY = mapH / 2;
  candidates.sort((a, b) => Math.hypot(a.x - midX, a.y - midY) - Math.hypot(b.x - midX, b.y - midY));
  const pick = candidates[0];
  map[pick.y][pick.x] = 5;
  return { x: pick.x + 0.5, y: pick.y + 0.5 };
}

function ensureExitDoor(level) {
  if (level.exitDoor) {
    const mx = Math.floor(level.exitDoor.x);
    const my = Math.floor(level.exitDoor.y);
    const isBorder = mx === 0 || my === 0 || mx === mapW - 1 || my === mapH - 1;
    if (isBorder) {
      map[my][mx] = 5;
      return { x: mx + 0.5, y: my + 0.5 };
    }
  }

  const existing = findBorderDoorCell();
  if (existing) {
    return existing;
  }

  const fallback = findFallbackBorderDoorCell();
  if (fallback) {
    return fallback;
  }

  // Last resort: force a border door on the right side.
  const forcedY = Math.floor(mapH / 2);
  map[forcedY][mapW - 1] = 5;
  return { x: mapW - 0.5, y: forcedY + 0.5 };
}

function ensureDoorIsReachableFromInside() {
  if (!exitDoor) {
    return;
  }

  const mx = Math.floor(exitDoor.x);
  const my = Math.floor(exitDoor.y);
  const neighbors = [
    { x: mx - 1, y: my },
    { x: mx + 1, y: my },
    { x: mx, y: my - 1 },
    { x: mx, y: my + 1 }
  ];
  let hasInnerOpen = false;
  for (const n of neighbors) {
    if (n.x > 0 && n.y > 0 && n.x < mapW - 1 && n.y < mapH - 1 && map[n.y][n.x] === 0) {
      hasInnerOpen = true;
      break;
    }
  }
  if (!hasInnerOpen) {
    if (mx === 0) {
      map[my][1] = 0;
    } else if (mx === mapW - 1) {
      map[my][mapW - 2] = 0;
    } else if (my === 0) {
      map[1][mx] = 0;
    } else if (my === mapH - 1) {
      map[mapH - 2][mx] = 0;
    }
  }
}

function collectHealthItems() {
  for (const item of healthItems) {
    if (item.collected || player.health >= PLAYER_HEALTH_MAX) {
      continue;
    }
    const dist = Math.hypot(player.x - item.x, player.y - item.y);
    if (dist <= HEALTH_ITEM_PICKUP_DIST) {
      item.collected = true;
      player.health = Math.min(PLAYER_HEALTH_MAX, player.health + HEALTH_ITEM_AMOUNT);
    }
  }
}

function castRay(rayAngle) {
  const rayDirX = Math.cos(rayAngle);
  const rayDirY = Math.sin(rayAngle);

  let mapX = Math.floor(player.x);
  let mapY = Math.floor(player.y);

  const deltaDistX = Math.abs(1 / (rayDirX || 0.0001));
  const deltaDistY = Math.abs(1 / (rayDirY || 0.0001));

  let sideDistX;
  let sideDistY;
  let stepX;
  let stepY;

  if (rayDirX < 0) {
    stepX = -1;
    sideDistX = (player.x - mapX) * deltaDistX;
  } else {
    stepX = 1;
    sideDistX = (mapX + 1 - player.x) * deltaDistX;
  }

  if (rayDirY < 0) {
    stepY = -1;
    sideDistY = (player.y - mapY) * deltaDistY;
  } else {
    stepY = 1;
    sideDistY = (mapY + 1 - player.y) * deltaDistY;
  }

  let hit = false;
  let side = 0;
  let steps = 0;

  while (!hit && steps < 128) {
    if (sideDistX < sideDistY) {
      sideDistX += deltaDistX;
      mapX += stepX;
      side = 0;
    } else {
      sideDistY += deltaDistY;
      mapY += stepY;
      side = 1;
    }
    if (isBlockingTileAt(mapX, mapY)) {
      hit = true;
    }
    steps += 1;
  }

  let dist;
  if (side === 0) {
    dist = (mapX - player.x + (1 - stepX) / 2) / (rayDirX || 0.0001);
  } else {
    dist = (mapY - player.y + (1 - stepY) / 2) / (rayDirY || 0.0001);
  }

  dist = Math.max(0.01, Math.min(MAX_DIST, dist));
  return { dist, side, mapX, mapY };
}

function normalizeAngle(angle) {
  let a = angle;
  while (a > Math.PI) a -= Math.PI * 2;
  while (a < -Math.PI) a += Math.PI * 2;
  return a;
}

function findEnemyHit(maxDist) {
  const forwardX = Math.cos(player.angle);
  const forwardY = Math.sin(player.angle);
  // 2D cross-product magnitude against look vector.
  const rightX = -forwardY;
  const rightY = forwardX;

  let best = null;
  for (const enemy of enemies) {
    if (!enemy.alive) {
      continue;
    }
    const dx = enemy.x - player.x;
    const dy = enemy.y - player.y;
    const along = dx * forwardX + dy * forwardY;
    if (along <= 0.05 || along >= maxDist) {
      continue;
    }

    const offset = Math.abs(dx * rightX + dy * rightY);
    if (offset > ENEMY_RADIUS) {
      continue;
    }

    if (!best || along < best.dist) {
      best = { enemy, dist: along };
    }
  }
  return best;
}

function hasLineOfSight(x0, y0, x1, y1) {
  const dx = x1 - x0;
  const dy = y1 - y0;
  const dist = Math.hypot(dx, dy);
  if (dist <= 0.0001) {
    return true;
  }

  const stepLen = 0.1;
  const stepX = (dx / dist) * stepLen;
  const stepY = (dy / dist) * stepLen;
  let x = x0;
  let y = y0;
  const steps = Math.ceil(dist / stepLen);

  for (let i = 0; i < steps; i += 1) {
    x += stepX;
    y += stepY;
    if (isWall(x, y)) {
      return false;
    }
  }
  return true;
}

function damagePlayer(amount) {
  if (player.dead) {
    return;
  }
  player.health = Math.max(0, player.health - amount);
  player.hurtTimer = 0.2;
  if (player.health <= 0) {
    player.dead = true;
  }
}

function updateEnemies(dt) {
  for (const enemy of enemies) {
    if (!enemy.alive) {
      continue;
    }

    const toPlayerX = player.x - enemy.x;
    const toPlayerY = player.y - enemy.y;
    const dist = Math.hypot(toPlayerX, toPlayerY);
    const seesPlayer = hasLineOfSight(enemy.x, enemy.y, player.x, player.y);

    if (seesPlayer && dist > ENEMY_STOP_RANGE) {
      const dirX = toPlayerX / Math.max(0.0001, dist);
      const dirY = toPlayerY / Math.max(0.0001, dist);
      const step = ENEMY_SPEED * dt;
      const nextX = enemy.x + dirX * step;
      const nextY = enemy.y + dirY * step;

      if (canEnemyOccupy(nextX, enemy.y)) {
        enemy.x = nextX;
      }
      if (canEnemyOccupy(enemy.x, nextY)) {
        enemy.y = nextY;
      }
    }

    if (seesPlayer && dist <= ENEMY_SHOOT_RANGE && enemy.fireCooldown <= 0) {
      enemy.fireCooldown = ENEMY_FIRE_COOLDOWN;
      damagePlayer(PLAYER_HIT_DAMAGE);
    }
  }
}

function tryShoot() {
  if (shootCooldown > 0 || player.dead || gameWon) {
    return;
  }

  shootCooldown = SHOT_COOLDOWN;
  muzzleFlashTimer = 0.07;

  const wallHit = castRay(player.angle);
  const enemyHit = findEnemyHit(wallHit.dist);
  if (enemyHit) {
    enemyHit.enemy.health -= 1;
    enemyHit.enemy.hitTimer = 0.12;
    if (enemyHit.enemy.health <= 0) {
      enemyHit.enemy.alive = false;
    }
    hitFlashTimer = 0.12;
    return;
  }

  if (wallHit.dist < MAX_DIST && wallHit.mapX >= 0 && wallHit.mapY >= 0 && wallHit.mapX < mapW && wallHit.mapY < mapH) {
    if (isBlockingTileAt(wallHit.mapX, wallHit.mapY)) {
      hitFlashTimer = 0.06;
    }
  }
}

function getWallBaseColor(tileType) {
  switch (tileType) {
    case 2:
      return { r: 70, g: 145, b: 230 };
    case 3:
      return { r: 84, g: 186, b: 109 };
    case 4:
      return { r: 179, g: 116, b: 235 };
    case 5:
      return exitDoorOpen ? { r: 102, g: 245, b: 152 } : { r: 245, g: 198, b: 71 };
    case 1:
    default:
      return { r: 178, g: 52, b: 46 };
  }
}

function renderFloorAndSky() {
  const half = canvas.height / 2;
  ctx.fillStyle = "#20253d";
  ctx.fillRect(0, 0, canvas.width, half);
  ctx.fillStyle = "#2b2b2b";
  ctx.fillRect(0, half, canvas.width, half);
}

function render3D() {
  const depthBuffer = new Array(canvas.width);
  renderFloorAndSky();

  for (let x = 0; x < canvas.width; x += 1) {
    const t = x / canvas.width;
    const rayAngle = player.angle - FOV / 2 + t * FOV;
    const ray = castRay(rayAngle);

    const correctedDist = ray.dist * Math.cos(rayAngle - player.angle);
    const lineHeight = Math.min(canvas.height, canvas.height / correctedDist);
    const drawStart = (canvas.height - lineHeight) / 2;

    const tileType =
      ray.mapX >= 0 && ray.mapY >= 0 && ray.mapX < mapW && ray.mapY < mapH
        ? map[ray.mapY][ray.mapX]
        : 1;
    const base = getWallBaseColor(tileType);
    const shade = ray.side ? 0.78 : 1;
    const fog = Math.max(0.2, 1 - correctedDist / MAX_DIST);
    const light = shade * fog;
    const r = Math.round(base.r * light);
    const g = Math.round(base.g * light);
    const b = Math.round(base.b * light);
    ctx.fillStyle = `rgb(${r}, ${g}, ${b})`;
    ctx.fillRect(x, drawStart, 1, lineHeight);
    depthBuffer[x] = correctedDist;
  }

  return depthBuffer;
}

function renderEnemies(depthBuffer) {
  const sorted = enemies
    .filter((enemy) => enemy.alive)
    .map((enemy) => {
      const dx = enemy.x - player.x;
      const dy = enemy.y - player.y;
      return { enemy, dist: Math.hypot(dx, dy) };
    })
    .sort((a, b) => b.dist - a.dist);

  for (const item of sorted) {
    const enemy = item.enemy;
    const dx = enemy.x - player.x;
    const dy = enemy.y - player.y;
    const dist = Math.max(0.01, Math.hypot(dx, dy));
    const enemyAngle = Math.atan2(dy, dx);
    const angleDiff = normalizeAngle(enemyAngle - player.angle);
    if (Math.abs(angleDiff) > FOV * 0.7) {
      continue;
    }

    const correctedDist = dist * Math.cos(angleDiff);
    if (correctedDist <= 0.05) {
      continue;
    }

    const screenX = (0.5 + angleDiff / FOV) * canvas.width;
    const spriteH = Math.min(canvas.height * 1.5, canvas.height / correctedDist);
    const spriteW = spriteH * 0.65;
    const top = canvas.height / 2 - spriteH / 2;
    const left = screenX - spriteW / 2;

    for (let x = 0; x < spriteW; x += 1) {
      const px = Math.floor(left + x);
      if (px < 0 || px >= canvas.width) {
        continue;
      }
      if (correctedDist > depthBuffer[px]) {
        continue;
      }

      const fog = Math.max(0.4, 1 - correctedDist / MAX_DIST);
      const hurtBoost = enemy.hitTimer > 0 ? 80 : 0;
      const hpFactor = Math.max(0.35, enemy.health / ENEMY_HEALTH);
      const edgeBand = Math.max(1, Math.floor(spriteW * 0.08));
      if (x < edgeBand || x > spriteW - edgeBand) {
        ctx.fillStyle = `rgb(${Math.round(15 * fog)}, ${Math.round(15 * fog)}, ${Math.round(15 * fog)})`;
        ctx.fillRect(px, top, 1, spriteH);
        continue;
      }

      const r = Math.min(255, Math.round((255 + hurtBoost) * fog));
      const g = Math.round((70 + 40 * (1 - hpFactor)) * fog);
      const b = Math.round((80 + 25 * (1 - hpFactor)) * fog);
      ctx.fillStyle = `rgb(${r}, ${g}, ${b})`;
      ctx.fillRect(px, top, 1, spriteH);
    }
  }
}

function renderHealthItems(depthBuffer) {
  const sorted = healthItems
    .filter((item) => !item.collected)
    .map((item) => {
      const dx = item.x - player.x;
      const dy = item.y - player.y;
      return { item, dist: Math.hypot(dx, dy) };
    })
    .sort((a, b) => b.dist - a.dist);

  for (const entry of sorted) {
    const item = entry.item;
    const dx = item.x - player.x;
    const dy = item.y - player.y;
    const dist = Math.max(0.01, Math.hypot(dx, dy));
    const itemAngle = Math.atan2(dy, dx);
    const angleDiff = normalizeAngle(itemAngle - player.angle);
    if (Math.abs(angleDiff) > FOV * 0.8) {
      continue;
    }

    const correctedDist = dist * Math.cos(angleDiff);
    if (correctedDist <= 0.05) {
      continue;
    }

    const screenX = (0.5 + angleDiff / FOV) * canvas.width;
    const spriteH = Math.min(canvas.height, canvas.height / correctedDist) * 0.45;
    const spriteW = spriteH * 0.8;
    const top = canvas.height / 2 + spriteH * 0.25;
    const left = screenX - spriteW / 2;

    for (let x = 0; x < spriteW; x += 1) {
      const px = Math.floor(left + x);
      if (px < 0 || px >= canvas.width) {
        continue;
      }
      if (correctedDist > depthBuffer[px]) {
        continue;
      }

      const fog = Math.max(0.45, 1 - correctedDist / MAX_DIST);
      const edgeBand = Math.max(1, Math.floor(spriteW * 0.16));
      const isEdge = x < edgeBand || x > spriteW - edgeBand;
      if (isEdge) {
        ctx.fillStyle = `rgb(${Math.round(18 * fog)}, ${Math.round(96 * fog)}, ${Math.round(18 * fog)})`;
      } else {
        ctx.fillStyle = `rgb(${Math.round(68 * fog)}, ${Math.round(230 * fog)}, ${Math.round(82 * fog)})`;
      }
      ctx.fillRect(px, top, 1, spriteH);
    }

    // Draw a bright white vertical stripe to imply a health cross.
    const crossX = Math.floor(screenX - 1);
    if (crossX >= 0 && crossX < canvas.width && correctedDist <= depthBuffer[crossX]) {
      const fog = Math.max(0.45, 1 - correctedDist / MAX_DIST);
      ctx.fillStyle = `rgb(${Math.round(240 * fog)}, ${Math.round(240 * fog)}, ${Math.round(240 * fog)})`;
      ctx.fillRect(crossX, top + spriteH * 0.18, 2, spriteH * 0.64);
    }
  }
}

function renderMinimap() {
  const scale = 12;
  const pad = 10;

  ctx.globalAlpha = 0.85;
  for (let y = 0; y < mapH; y += 1) {
    for (let x = 0; x < mapW; x += 1) {
      const tile = map[y][x];
      if (tile === 0) {
        ctx.fillStyle = "#1a1a1a";
      } else if (tile === 1) {
        ctx.fillStyle = "#8b3c36";
      } else if (tile === 2) {
        ctx.fillStyle = "#4d7db8";
      } else if (tile === 3) {
        ctx.fillStyle = "#4f965f";
      } else if (tile === 5) {
        ctx.fillStyle = exitDoorOpen ? "#63d88f" : "#d7a543";
      } else {
        ctx.fillStyle = "#8f64ba";
      }
      ctx.fillRect(pad + x * scale, pad + y * scale, scale - 1, scale - 1);
    }
  }

  for (const enemy of enemies) {
    if (!enemy.alive) {
      continue;
    }
    ctx.fillStyle = enemy.hitTimer > 0 ? "#ffcc66" : "#ff5555";
    ctx.beginPath();
    ctx.arc(pad + enemy.x * scale, pad + enemy.y * scale, 3, 0, Math.PI * 2);
    ctx.fill();
  }

  for (const item of healthItems) {
    if (item.collected) {
      continue;
    }
    ctx.fillStyle = "#5cff7a";
    ctx.beginPath();
    ctx.arc(pad + item.x * scale, pad + item.y * scale, 2.6, 0, Math.PI * 2);
    ctx.fill();
  }

  ctx.fillStyle = "#4caf50";
  ctx.beginPath();
  ctx.arc(pad + player.x * scale, pad + player.y * scale, 3, 0, Math.PI * 2);
  ctx.fill();

  ctx.strokeStyle = "#8dd4ff";
  ctx.beginPath();
  ctx.moveTo(pad + player.x * scale, pad + player.y * scale);
  ctx.lineTo(
    pad + (player.x + Math.cos(player.angle) * 1.5) * scale,
    pad + (player.y + Math.sin(player.angle) * 1.5) * scale
  );
  ctx.stroke();
  ctx.globalAlpha = 1;
}

function renderWeaponAndCrosshair() {
  const cx = canvas.width / 2;
  const cy = canvas.height / 2;

  ctx.strokeStyle = hitFlashTimer > 0 ? "#ffe066" : "#f2f2f2";
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(cx - 9, cy);
  ctx.lineTo(cx - 2, cy);
  ctx.moveTo(cx + 2, cy);
  ctx.lineTo(cx + 9, cy);
  ctx.moveTo(cx, cy - 9);
  ctx.lineTo(cx, cy - 2);
  ctx.moveTo(cx, cy + 2);
  ctx.lineTo(cx, cy + 9);
  ctx.stroke();

  // Very simple "weapon" shape at the bottom center.
  const baseW = 150;
  const baseH = 70;
  ctx.fillStyle = "#1f1f1f";
  ctx.fillRect(cx - baseW / 2, canvas.height - baseH, baseW, baseH);
  ctx.fillStyle = "#2c2c2c";
  ctx.fillRect(cx - 18, canvas.height - baseH - 30, 36, 34);

  if (muzzleFlashTimer > 0) {
    ctx.fillStyle = "#ffd54d";
    ctx.beginPath();
    ctx.moveTo(cx, canvas.height - baseH - 50);
    ctx.lineTo(cx - 16, canvas.height - baseH - 24);
    ctx.lineTo(cx + 16, canvas.height - baseH - 24);
    ctx.closePath();
    ctx.fill();
  }
}

function renderHud() {
  const aliveCount = enemies.filter((enemy) => enemy.alive).length;
  const medkitCount = healthItems.filter((item) => !item.collected).length;

  ctx.fillStyle = "rgba(0, 0, 0, 0.45)";
  ctx.fillRect(10, canvas.height - 44, 300, 30);

  ctx.fillStyle = "#f0f0f0";
  ctx.font = "16px Arial, sans-serif";
  ctx.fillText(`Health: ${player.health}`, 16, canvas.height - 24);
  ctx.fillText(`Enemies: ${aliveCount}`, 120, canvas.height - 24);
  ctx.fillText(`Medkits: ${medkitCount}`, 215, canvas.height - 24);
  ctx.fillText(`Map: ${currentLevelIndex + 1}/${LEVELS.length}`, 16, 28);
  const doorText = doorStatusText();
  if (doorText) {
    ctx.fillText(doorText, 150, 28);
  }

  if (player.hurtTimer > 0) {
    ctx.fillStyle = "rgba(255, 70, 70, 0.18)";
    ctx.fillRect(0, 0, canvas.width, canvas.height);
  }

  if (player.dead) {
    ctx.fillStyle = "rgba(0, 0, 0, 0.65)";
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.fillStyle = "#ff7474";
    ctx.font = "bold 44px Arial, sans-serif";
    ctx.textAlign = "center";
    ctx.fillText("You Died", canvas.width / 2, canvas.height / 2 - 8);
    ctx.font = "20px Arial, sans-serif";
    ctx.fillStyle = "#f1f1f1";
    ctx.fillText("Press R to restart", canvas.width / 2, canvas.height / 2 + 26);
    ctx.textAlign = "start";
  }

  if (gameWon) {
    ctx.fillStyle = "rgba(0, 0, 0, 0.65)";
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.fillStyle = "#8eff9a";
    ctx.font = "bold 42px Arial, sans-serif";
    ctx.textAlign = "center";
    ctx.fillText("You Win!", canvas.width / 2, canvas.height / 2 - 10);
    ctx.font = "20px Arial, sans-serif";
    ctx.fillStyle = "#f1f1f1";
    ctx.fillText("All maps cleared - press R to restart", canvas.width / 2, canvas.height / 2 + 24);
    ctx.textAlign = "start";
  }
}

function loadLevel(levelIndex) {
  const level = LEVELS[levelIndex];
  map = level.map;
  mapW = map[0].length;
  mapH = map.length;
  exitDoorOpen = false;
  exitDoor = ensureExitDoor(level);
  ensureDoorIsReachableFromInside();

  enemies = [];
  for (const enemy of level.enemies) {
    const spawn = findNearestValidSpawn(enemy.x, enemy.y, isEnemySpawnOpen);
    if (!spawn) {
      continue;
    }
    enemies.push({
      x: spawn.x,
      y: spawn.y,
      health: ENEMY_HEALTH,
      hitTimer: 0,
      alive: true,
      fireCooldown: 0
    });
  }

  healthItems = [];
  for (const item of level.healthItems || []) {
    const spawn = findNearestValidSpawn(item.x, item.y, isItemSpawnOpen);
    if (!spawn) {
      continue;
    }
    healthItems.push({
      x: spawn.x,
      y: spawn.y,
      collected: false
    });
  }

  player.x = level.playerStart.x;
  player.y = level.playerStart.y;
  player.angle = level.playerStart.angle;
  player.hurtTimer = 0;
  shootCooldown = 0;
  muzzleFlashTimer = 0;
  hitFlashTimer = 0;
  placePlayerInFreeCell();
}

function advanceToNextLevel() {
  if (currentLevelIndex < LEVELS.length - 1) {
    currentLevelIndex += 1;
    loadLevel(currentLevelIndex);
  } else {
    gameWon = true;
  }
}

function restartGame() {
  player.dead = false;
  player.health = PLAYER_HEALTH_MAX;
  currentLevelIndex = 0;
  gameWon = false;
  loadLevel(currentLevelIndex);
}

function frame(now) {
  const dt = Math.min(0.05, (now - lastTime) / 1000);
  lastTime = now;

  update(dt);
  const depthBuffer = render3D();
  renderEnemies(depthBuffer);
  renderHealthItems(depthBuffer);
  renderMinimap();
  renderWeaponAndCrosshair();
  renderHud();

  requestAnimationFrame(frame);
}

loadLevel(currentLevelIndex);
requestAnimationFrame(frame);
