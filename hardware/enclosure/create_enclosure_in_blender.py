import bpy
import bmesh
import math

# Clear existing objects
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete()

# Clear existing materials
for mat in bpy.data.materials:
    bpy.data.materials.remove(mat)

# --- Parameters (in meters for Blender, so mm * 0.001) ---
pcb_length = 53.0 * 0.001
pcb_width = 28.0 * 0.001
pcb_thickness = 1.6 * 0.001

clearance_xy = 1.0 * 0.001
clearance_z_bottom = 2.0 * 0.001
clearance_z_top = 5.0 * 0.001 # INCREASED: Add more top clearance so the rim is taller and doesn't break

wall_thickness = 2.0 * 0.001
base_thickness = 2.0 * 0.001
corner_radius = 3.0 * 0.001

# Type-C Ports dimensions
port_width = 9.5 * 0.001
port_height = 4.0 * 0.001
port_spacing = 13.0 * 0.001
# Lower the port by reducing the clearance component or shifting the center down slightly.
# We shift it down by 0.5mm so it doesn't break the top edge if the board is placed low.
port_center_z = clearance_z_bottom + pcb_thickness + (port_height/2) - (0.5 * 0.001)

pillar_outer_d = 5.0 * 0.001
pillar_inner_d = 1.8 * 0.001
screw_hole_offset = max(3.0 * 0.001, corner_radius)
cover_thickness = 2.0 * 0.001

inner_length = pcb_length + (clearance_xy * 2)
inner_width = pcb_width + (clearance_xy * 2)
inner_depth = clearance_z_bottom + pcb_thickness + clearance_z_top

outer_length = inner_length + (wall_thickness * 2)
outer_width = inner_width + (wall_thickness * 2)
outer_height = inner_depth + base_thickness

def create_cylinder(name, location, radius, depth):
    bpy.ops.mesh.primitive_cylinder_add(vertices=32, radius=radius, depth=depth, location=location)
    obj = bpy.context.active_object
    obj.name = name
    return obj

def apply_boolean(target, tool, operation='DIFFERENCE'):
    # Move the tool to a new collection to hide it just in case, though we delete it
    bool_mod = target.modifiers.new(name="Boolean", type='BOOLEAN')
    bool_mod.operation = operation
    bool_mod.object = tool
    # EXACT solver sometimes fails with perfectly aligned edges, FAST is often safer for simple box cuts
    bool_mod.solver = 'FAST' if operation == 'DIFFERENCE' else 'EXACT'
    
    # Apply modifier
    bpy.context.view_layer.objects.active = target
    bpy.ops.object.modifier_apply(modifier="Boolean")
    
    # Delete tool
    bpy.data.objects.remove(tool, do_unlink=True)

def get_principled_bsdf(mat):
    if not mat.use_nodes:
        mat.use_nodes = True
    for node in mat.node_tree.nodes:
        if node.type == 'BSDF_PRINCIPLED':
            return node
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    for node in nodes:
        nodes.remove(node)
    output_node = nodes.new(type='ShaderNodeOutputMaterial')
    bsdf_node = nodes.new(type='ShaderNodeBsdfPrincipled')
    links.new(bsdf_node.outputs['BSDF'], output_node.inputs['Surface'])
    return bsdf_node

def create_aluminum_material():
    mat = bpy.data.materials.new(name="Aluminum_SLM")
    bsdf = get_principled_bsdf(mat)
    if bsdf:
        bsdf.inputs['Base Color'].default_value = (0.4, 0.4, 0.45, 1)
        bsdf.inputs['Metallic'].default_value = 1.0
        bsdf.inputs['Roughness'].default_value = 0.35
    return mat

def create_acrylic_material():
    mat = bpy.data.materials.new(name="Black_Translucent_Acrylic")
    mat.blend_method = 'BLEND' 
    mat.shadow_method = 'HASHED'
    bsdf = get_principled_bsdf(mat)
    if bsdf:
        bsdf.inputs['Base Color'].default_value = (0.02, 0.02, 0.02, 1)
        bsdf.inputs['Metallic'].default_value = 0.0
        bsdf.inputs['Roughness'].default_value = 0.05
        bsdf.inputs['Alpha'].default_value = 0.4
    return mat

mat_aluminum = create_aluminum_material()
mat_acrylic = create_acrylic_material()

def create_rounded_box(name, length, width, height, radius, z_offset=0, x_offset=0, y_offset=0):
    center_loc = (length/2 + x_offset, width/2 + y_offset, height/2 + z_offset)
    
    bpy.ops.mesh.primitive_cube_add(size=1, location=center_loc)
    cross1 = bpy.context.active_object
    cross1.scale = (length, width - 2*radius, height)
    
    bpy.ops.mesh.primitive_cube_add(size=1, location=center_loc)
    cross2 = bpy.context.active_object
    cross2.scale = (length - 2*radius, width, height)
    
    bool_mod = cross1.modifiers.new(name="Union", type='BOOLEAN')
    bool_mod.operation = 'UNION'
    bool_mod.object = cross2
    bool_mod.solver = 'EXACT'
    bpy.context.view_layer.objects.active = cross1
    bpy.ops.object.modifier_apply(modifier="Union")
    bpy.data.objects.remove(cross2, do_unlink=True)
    
    cylinders = []
    for dx in [1, -1]:
        for dy in [1, -1]:
            cx = length/2 + dx * (length/2 - radius) + x_offset
            cy = width/2 + dy * (width/2 - radius) + y_offset
            cyl = create_cylinder("Corner", (cx, cy, height/2 + z_offset), radius, height)
            cylinders.append(cyl)
            
    for cyl in cylinders:
        bool_mod = cross1.modifiers.new(name="Union", type='BOOLEAN')
        bool_mod.operation = 'UNION'
        bool_mod.object = cyl
        bool_mod.solver = 'EXACT'
        bpy.context.view_layer.objects.active = cross1
        bpy.ops.object.modifier_apply(modifier="Union")
        bpy.data.objects.remove(cyl, do_unlink=True)
        
    cross1.name = name
    return cross1

def create_cube_simple(name, location, scale):
    bpy.ops.mesh.primitive_cube_add(size=1, location=location)
    obj = bpy.context.active_object
    obj.name = name
    obj.scale = scale
    return obj


# --- 1. Base Shell ---
# Outer Shell
base_outer = create_rounded_box("Aluminum_Base", outer_length, outer_width, outer_height, corner_radius, 0)

# Inner Cavity Cutter
inner_radius = max(0.1 * 0.001, corner_radius - wall_thickness) 
# Cutter needs to be much taller to ensure the TOP is completely open!
cutter_height = inner_depth + 10.0 * 0.001 # Make it massively tall so it cuts all the way through the ceiling
base_inner = create_rounded_box("Base_Inner", inner_length, inner_width, cutter_height, inner_radius, base_thickness, wall_thickness, wall_thickness)

# Cut cavity
bpy.context.view_layer.update()
apply_boolean(base_outer, base_inner, 'DIFFERENCE')

# --- PORTS CUTOUT FIX ---
# Instead of cutting *into* the base after cutting the cavity, 
# we should make sure we subtract a cube that is definitely intersecting the wall.

# To prevent the USB port cutout from "cutting up through the top rim",
# we need to make sure the cutout cutter box does not extend too high.
# Actually, the user says "底部上面太突了，不应该顶到上面的盖子", this means
# the cutout we made went all the way up and broke the top rim of the aluminum base!
# Let's ensure the cutter is ONLY as tall as port_height, and not too high.
# But wait, maybe the user means the *ports* are placed too high on the Z axis?
# port_center_z = clearance_z_bottom + pcb_thickness + (port_height/2) - (0.5 * 0.001)

# Let's create an "open slot" style cutout if that's what the user wants, or just a lower hole.
# Let's make the hole a slot that goes up, BUT stops before the top edge.
# Actually, the cutter is just a cube of height port_height (4mm).
# If it's hitting the top, our inner_depth is too small or port_z is too high.
port_z = base_thickness + clearance_z_bottom + pcb_thickness + (port_height/2) - (1.0 * 0.001)
port_cutter_length = wall_thickness * 10.0 # Massively long

cutter_x_loc = 0.0 # Put the center exactly at X=0, so it cuts outwards and inwards

port1_loc = (cutter_x_loc, wall_thickness + inner_width/2 - port_spacing/2, port_z)
port1 = create_cube_simple("Port1_Cutter", port1_loc, (port_cutter_length, port_width, port_height))

port2_loc = (cutter_x_loc, wall_thickness + inner_width/2 + port_spacing/2, port_z)
port2 = create_cube_simple("Port2_Cutter", port2_loc, (port_cutter_length, port_width, port_height))

# Force Blender to update scene before booleans
bpy.context.view_layer.update()

apply_boolean(base_outer, port1, 'DIFFERENCE')
apply_boolean(base_outer, port2, 'DIFFERENCE')
# --- END PORTS CUTOUT FIX ---

# Pillars
pillar_h = inner_depth
for dx, dy in [(1, 1), (-1, 1), (1, -1), (-1, -1)]:
    cx = wall_thickness + screw_hole_offset if dx == 1 else outer_length - wall_thickness - screw_hole_offset
    cy = wall_thickness + screw_hole_offset if dy == 1 else outer_width - wall_thickness - screw_hole_offset
    
    pillar = create_cylinder("Pillar", (cx, cy, base_thickness + pillar_h/2), pillar_outer_d/2, pillar_h)
    hole = create_cylinder("Hole", (cx, cy, base_thickness + pillar_h/2), pillar_inner_d/2, pillar_h + 0.002)
    apply_boolean(pillar, hole)
    
    bool_mod = base_outer.modifiers.new(name="Union", type='BOOLEAN')
    bool_mod.operation = 'UNION'
    bool_mod.object = pillar
    bool_mod.solver = 'EXACT'
    bpy.context.view_layer.objects.active = base_outer
    bpy.ops.object.modifier_apply(modifier="Union")
    bpy.data.objects.remove(pillar, do_unlink=True)

# Ledges for PCB support
ledge_w = 4.0 * 0.001
ledge_h = clearance_z_bottom
for dx, dy in [(1, 1), (-1, 1), (1, -1), (-1, -1)]:
    cx = wall_thickness + ledge_w/2 if dx == 1 else outer_length - wall_thickness - ledge_w/2
    cy = wall_thickness + ledge_w/2 if dy == 1 else outer_width - wall_thickness - ledge_w/2
    
    ledge = create_cube_simple("Ledge", (cx, cy, base_thickness + ledge_h/2), (ledge_w, ledge_w, ledge_h))
    
    bool_mod = base_outer.modifiers.new(name="Union", type='BOOLEAN')
    bool_mod.operation = 'UNION'
    bool_mod.object = ledge
    bool_mod.solver = 'EXACT'
    bpy.context.view_layer.objects.active = base_outer
    bpy.ops.object.modifier_apply(modifier="Union")
    bpy.data.objects.remove(ledge, do_unlink=True)


# Add Top/Bottom Edge Bevels safely
bevel_mod = base_outer.modifiers.new(name="Edge Bevel", type='BEVEL')
bevel_mod.segments = 3
bevel_mod.width = 0.5 * 0.001
bevel_mod.limit_method = 'ANGLE'
bevel_mod.angle_limit = math.radians(89) 

# Apply Aluminum Material
if len(base_outer.data.materials) == 0:
    base_outer.data.materials.append(mat_aluminum)
else:
    base_outer.data.materials[0] = mat_aluminum

bpy.context.view_layer.objects.active = base_outer
bpy.ops.object.shade_smooth() 

# --- 2. Top Cover ---
cover_loc_z = outer_height + cover_thickness/2 + 0.01
cover = create_rounded_box("Acrylic_Top_Cover", outer_length, outer_width, cover_thickness, corner_radius, cover_loc_z - cover_thickness/2)

cover_bevel = cover.modifiers.new(name="Edge Bevel", type='BEVEL')
cover_bevel.segments = 3
cover_bevel.width = 0.5 * 0.001
cover_bevel.limit_method = 'ANGLE'
cover_bevel.angle_limit = math.radians(89)

# Screw Holes in cover
for dx, dy in [(1, 1), (-1, 1), (1, -1), (-1, -1)]:
    cx = wall_thickness + screw_hole_offset if dx == 1 else outer_length - wall_thickness - screw_hole_offset
    cy = wall_thickness + screw_hole_offset if dy == 1 else outer_width - wall_thickness - screw_hole_offset
    
    hole = create_cylinder("Cover_Hole", (cx, cy, cover_loc_z), 2.5 * 0.001 / 2, cover_thickness + 0.002)
    apply_boolean(cover, hole)

# Apply Acrylic Material
if len(cover.data.materials) == 0:
    cover.data.materials.append(mat_acrylic)
else:
    cover.data.materials[0] = mat_acrylic

bpy.context.view_layer.objects.active = cover
bpy.ops.object.shade_smooth() 

# Add a basic light
bpy.ops.object.light_add(type='POINT', radius=1, align='WORLD', location=(0, 0, 0.2))
light = bpy.context.active_object
light.data.energy = 50.0

# Add a floor to reflect
bpy.ops.mesh.primitive_plane_add(size=0.5, enter_editmode=False, align='WORLD', location=(outer_length/2, outer_width/2, -0.005))
floor = bpy.context.active_object
mat_floor = bpy.data.materials.new(name="Floor")
bsdf_floor = get_principled_bsdf(mat_floor)
if bsdf_floor:
    bsdf_floor.inputs['Base Color'].default_value = (0.8, 0.8, 0.8, 1)
    bsdf_floor.inputs['Roughness'].default_value = 0.1
floor.data.materials.append(mat_floor)

# Reset origin
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.origin_set(type='ORIGIN_CURSOR')

# Force Viewport to Material Preview mode
try:
    for area in bpy.context.screen.areas:
        if area.type == 'VIEW_3D':
            for space in area.spaces:
                if space.type == 'VIEW_3D':
                    space.shading.type = 'MATERIAL'
                    space.shading.use_scene_world_lookdev = False
                    space.shading.use_scene_lights_lookdev = False
except Exception as e:
    print(f"Viewport setting error: {e}")

print("Enclosure generated with fixed port holes successfully!")
