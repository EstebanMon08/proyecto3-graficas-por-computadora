# Codigo generado con ayuda de un LLM (Claude, Anthropic).
#
# Uso: abrir el .blend con todos los modelos > pestana Scripting > Open este archivo >
# editar SALIDA y MODELOS > Run Script (Alt+P).
#
# Por cada modelo escribe SALIDA/<Archivo>.obj, SALIDA/<Archivo>.mtl y un .png por material.
#  - Material con Image Texture conectada directo al Base Color: guarda esa imagen como PNG.
#  - Material con nodos procedurales u otros nodos: lo hornea (bake) a PNG con Cycles.
#  - Material con color plano: genera un PNG pequeno (4x4) de ese color.
# Solo se leen materiales con Principled BSDF. Los cambios a nodos se revierten al terminar,
# pero si un modelo no tiene UVs, se le agrega un UV Map (Smart UV Project).

import os
import re
import bpy
import mathutils

SALIDA = r"C:\Documentos\UVG\Tercer Año\Segundo Semestre\Graficos a Computador\Proyecto 3\assets"
TAM_BAKE = 1024

# "NombreDeArchivo": "nombre del objeto o coleccion en Blender"
# (no importan mayusculas, espacios, guiones ni guiones bajos)
MODELOS = {
    "AgujeroNegro": "AgujeroNegro",
    "PuzzlePlank": "PuzzlePlank",
    "PopStar": "PopStar",
    "ShiverStar": "ShiverStar",
    "Moon": "Moon",
    "CosmicCove": "CosmicCove",
    "RollingMasterpiece": "RollingMasterpiece",
    "Spaceship": "Spaceship",
}


def norm(s):
    return re.sub(r"[\s_\-\.]", "", s.lower())


def seguro(s):
    return re.sub(r"[^A-Za-z0-9_\-]", "_", s)


def buscar_mallas(nombre):
    n = norm(nombre)
    for col in bpy.data.collections:
        if norm(col.name) == n:
            return [o for o in col.all_objects if o.type == "MESH"]
    for o in bpy.data.objects:
        if norm(o.name) == n:
            objs = [o] + list(o.children_recursive)
            return [x for x in objs if x.type == "MESH"]
    return []


def principled(mat):
    if mat is None or not mat.use_nodes or mat.node_tree is None:
        return None
    for n in mat.node_tree.nodes:
        if n.type == "BSDF_PRINCIPLED":
            return n
    return None


def guardar_png_copia(img, ruta):
    copia = img.copy()
    try:
        copia.filepath_raw = ruta
        copia.file_format = "PNG"
        copia.save()
    except Exception:
        img.save_render(ruta)
    finally:
        bpy.data.images.remove(copia)


def asegurar_uv(objs):
    for o in objs:
        if o.data.uv_layers:
            continue
        bpy.ops.object.select_all(action="DESELECT")
        o.select_set(True)
        bpy.context.view_layer.objects.active = o
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.uv.smart_project()
        bpy.ops.object.mode_set(mode="OBJECT")


def preparar_materiales(objs, carpeta, estado):
    materiales = []
    for o in objs:
        for slot in o.material_slots:
            if slot.material and slot.material not in materiales:
                materiales.append(slot.material)

    por_hornear = []
    for mat in materiales:
        p = principled(mat)
        if p is None:
            continue
        entrada = p.inputs["Base Color"]
        nombre_viejo = mat.name
        mat.name = seguro(mat.name)
        estado.append(("mat", mat, nombre_viejo))
        ruta = os.path.join(carpeta, mat.name + ".png")
        if not entrada.is_linked:
            c = entrada.default_value
            srgb = mathutils.Color((c[0], c[1], c[2])).from_scene_linear_to_srgb()
            img = bpy.data.images.new(mat.name + "_color", 4, 4)
            img.pixels = [srgb.r, srgb.g, srgb.b, 1.0] * 16
            img.filepath_raw = ruta
            img.file_format = "PNG"
            img.save()
            nt = mat.node_tree
            nodo = nt.nodes.new("ShaderNodeTexImage")
            nodo.image = img
            nt.links.new(nodo.outputs["Color"], entrada)
            estado.append(("bake", nt, entrada, None, nodo, img))
            print("  PNG de color plano:", ruta)
            continue
        origen = entrada.links[0].from_node
        if origen.type == "TEX_IMAGE" and origen.image is not None and origen.image.size[0] > 0:
            guardar_png_copia(origen.image, ruta)
            original = origen.image
            origen.image = bpy.data.images.load(ruta, check_existing=False)
            estado.append(("img", origen, original))
            print("  PNG guardado:", ruta)
        else:
            por_hornear.append((mat, p, entrada, ruta))

    if not por_hornear:
        return

    asegurar_uv(objs)
    motor_viejo = bpy.context.scene.render.engine
    bpy.context.scene.render.engine = "CYCLES"
    estado.append(("motor", motor_viejo))

    nodos = []
    for mat, p, entrada, ruta in por_hornear:
        nt = mat.node_tree
        img = bpy.data.images.new(mat.name + "_bake", TAM_BAKE, TAM_BAKE)
        nodo = nt.nodes.new("ShaderNodeTexImage")
        nodo.image = img
        for n in nt.nodes:
            n.select = False
        nodo.select = True
        nt.nodes.active = nodo
        nodos.append((mat, entrada, ruta, nodo, img))

    bpy.ops.object.select_all(action="DESELECT")
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.object.bake(type="DIFFUSE", pass_filter={"COLOR"}, margin=8)

    for mat, entrada, ruta, nodo, img in nodos:
        img.filepath_raw = ruta
        img.file_format = "PNG"
        img.save()
        print("  PNG horneado:", ruta)
        nt = mat.node_tree
        previo = entrada.links[0].from_socket
        nt.links.remove(entrada.links[0])
        nt.links.new(nodo.outputs["Color"], entrada)
        estado.append(("bake", nt, entrada, previo, nodo, img))


def restaurar(estado):
    for e in reversed(estado):
        try:
            if e[0] == "bake":
                _, nt, entrada, previo, nodo, img = e
                if entrada.is_linked:
                    nt.links.remove(entrada.links[0])
                if previo is not None:
                    nt.links.new(previo, entrada)
                nt.nodes.remove(nodo)
                bpy.data.images.remove(img)
            elif e[0] == "img":
                _, nodo, original = e
                tmp = nodo.image
                nodo.image = original
                if tmp is not None:
                    bpy.data.images.remove(tmp)
            elif e[0] == "mat":
                e[1].name = e[2]
            elif e[0] == "motor":
                bpy.context.scene.render.engine = e[1]
        except Exception as ex:
            print("  aviso al restaurar:", ex)


def exportar(objs, ruta_obj):
    bpy.ops.object.select_all(action="DESELECT")
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    if hasattr(bpy.ops.wm, "obj_export"):
        bpy.ops.wm.obj_export(
            filepath=ruta_obj,
            export_selected_objects=True,
            export_materials=True,
            path_mode="STRIP",
            forward_axis="NEGATIVE_Z",
            up_axis="Y",
            export_triangulated_mesh=False,
            export_uv=True,
            export_normals=True,
            apply_modifiers=True,
        )
    else:
        bpy.ops.export_scene.obj(
            filepath=ruta_obj,
            use_selection=True,
            use_materials=True,
            path_mode="STRIP",
            axis_forward="-Z",
            axis_up="Y",
            use_triangles=False,
            use_normals=True,
            use_uvs=True,
        )


def main():
    os.makedirs(SALIDA, exist_ok=True)
    if bpy.context.object and bpy.context.object.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    for archivo, nombre in MODELOS.items():
        objs = buscar_mallas(nombre)
        if not objs:
            print("NO ENCONTRADO:", nombre, "(archivo", archivo + ")")
            continue
        print("Exportando", archivo, "-", len(objs), "objeto(s)")
        estado = []
        try:
            preparar_materiales(objs, SALIDA, estado)
            exportar(objs, os.path.join(SALIDA, archivo + ".obj"))
        finally:
            restaurar(estado)
    print("Listo. Archivos en:", SALIDA)


main()
