#include "vtk.hpp"

#include <fstream>

#ifdef OMEGA_H_USE_ZLIB
#include <zlib.h>
#endif

#include "access.hpp"
#include "base64.hpp"
#include "file.hpp"
#include "simplices.hpp"
#include "tag.hpp"
#include "xml.hpp"

namespace Omega_h {

namespace vtk {

namespace {

/* start of C++ ritual dance to print a string based on
   type properties */

template <bool is_signed, std::size_t size>
struct IntTraits;

template <>
struct IntTraits<true, 1> {
  inline static char const* name() { return "Int8"; }
};

template <>
struct IntTraits<true, 4> {
  inline static char const* name() { return "Int32"; }
};

template <>
struct IntTraits<true, 8> {
  inline static char const* name() { return "Int64"; }
};

template <>
struct IntTraits<false, 8> {
  inline static char const* name() { return "UInt64"; }
};

template <std::size_t size>
struct FloatTraits;

template <>
struct FloatTraits<8> {
  inline static char const* name() { return "Float64"; }
};

template <typename T, typename Enable = void>
struct Traits;

template <typename T>
struct Traits<T, typename std::enable_if<std::is_integral<T>::value>::type> {
  inline static char const* name() {
    return IntTraits<std::is_signed<T>::value, sizeof(T)>::name();
  }
};

template <typename T>
struct Traits<T,
    typename std::enable_if<std::is_floating_point<T>::value>::type> {
  inline static char const* name() { return FloatTraits<sizeof(T)>::name(); }
};

/* end of C++ ritual dance to get a string based on type properties */

template <typename T>
void describe_array(std::ostream& stream, std::string const& name, Int ncomps) {
  stream << "type=\"" << Traits<T>::name() << "\"";
  stream << " Name=\"" << name << "\"";
  stream << " NumberOfComponents=\"" << ncomps << "\"";
  stream << " format=\"binary\"";
}

bool read_array_start_tag(std::istream& stream, Omega_h_Type* type_out,
    std::string* name_out, Int* ncomps_out) {
  auto st = xml::read_tag(stream);
  if (st.elem_name != "DataArray" || st.type != xml::Tag::START) {
    CHECK(st.type == xml::Tag::END);
    return false;
  }
  auto type_name = st.attribs["type"];
  if (type_name == "Int8")
    *type_out = OMEGA_H_I8;
  else if (type_name == "Int32")
    *type_out = OMEGA_H_I32;
  else if (type_name == "Int64")
    *type_out = OMEGA_H_I64;
  else if (type_name == "Float64")
    *type_out = OMEGA_H_F64;
  *name_out = st.attribs["Name"];
  *ncomps_out = std::stoi(st.attribs["NumberOfComponents"]);
  CHECK(st.attribs["format"] == "binary");
  return true;
}

template <typename T>
void write_array(
    std::ostream& stream, std::string const& name, Int ncomps, Read<T> array) {
  if (!(array.exists())) {
    Omega_h_fail("vtk::write_array: \"%s\" doesn't exist\n", name.c_str());
  }
  stream << "<DataArray ";
  describe_array<T>(stream, name, ncomps);
  stream << ">\n";
  HostRead<T> uncompressed(array);
  std::size_t uncompressed_bytes =
      sizeof(T) * static_cast<std::size_t>(array.size());
#ifdef OMEGA_H_USE_ZLIB
  uLong source_bytes = uncompressed_bytes;
  uLong dest_bytes = ::compressBound(source_bytes);
  auto compressed = new Bytef[dest_bytes];
  int ret = ::compress2(compressed, &dest_bytes,
      reinterpret_cast<const Bytef*>(uncompressed.data()), source_bytes,
      Z_BEST_SPEED);
  CHECK(ret == Z_OK);
  std::string encoded = base64::encode(compressed, dest_bytes);
  delete[] compressed;
  std::size_t header[4] = {
      1, uncompressed_bytes, uncompressed_bytes, dest_bytes};
  std::string enc_header = base64::encode(header, sizeof(header));
#else
  std::string enc_header =
      base64::encode(&uncompressed_bytes, sizeof(std::size_t));
  std::string encoded = base64::encode(uncompressed.data(), uncompressed_bytes);
#endif
  stream << enc_header << encoded << '\n';
  stream << "</DataArray>\n";
}

template <typename T>
Read<T> read_array(
    std::istream& stream, LO size, bool is_little_endian, bool is_compressed) {
  auto enc_both = base64::read_encoded(stream);
  std::size_t uncompressed_bytes, compressed_bytes;
  std::string encoded;
#ifdef OMEGA_H_USE_ZLIB
  if (is_compressed) {
    std::size_t header[4];
    auto nheader_chars = base64::encoded_size(sizeof(header));
    auto enc_header = enc_both.substr(0, nheader_chars);
    base64::decode(enc_header, header, sizeof(header));
    for (std::size_t i = 0; i < 4; ++i) {
      binary::swap_if_needed(header[i], is_little_endian);
    }
    encoded = enc_both.substr(nheader_chars);
    uncompressed_bytes = header[2];
    compressed_bytes = header[3];
  } else
#else
  CHECK(is_compressed == false);
#endif
  {
    auto nheader_chars = base64::encoded_size(sizeof(std::size_t));
    auto enc_header = enc_both.substr(0, nheader_chars);
    base64::decode(enc_header, &uncompressed_bytes, sizeof(uncompressed_bytes));
    binary::swap_if_needed(uncompressed_bytes, is_little_endian);
    compressed_bytes = uncompressed_bytes;
    encoded = enc_both.substr(nheader_chars);
  }
  CHECK(uncompressed_bytes == std::size_t(size) * sizeof(T));
  HostWrite<T> uncompressed(size);
#ifdef OMEGA_H_USE_ZLIB
  if (is_compressed) {
    auto compressed = new Bytef[compressed_bytes];
    base64::decode(encoded, compressed, compressed_bytes);
    uLong dest_bytes = static_cast<uLong>(uncompressed_bytes);
    uLong source_bytes = static_cast<uLong>(compressed_bytes);
    Bytef* uncompressed_ptr = reinterpret_cast<Bytef*>(uncompressed.data());
    int ret =
        ::uncompress(uncompressed_ptr, &dest_bytes, compressed, source_bytes);
    if (ret != Z_OK) {
      Omega_h_fail(
          "code %d: couln't decompress \"%s\"\n", ret, encoded.c_str());
    }
    CHECK(dest_bytes == static_cast<uLong>(uncompressed_bytes));
    delete[] compressed;
  } else
#endif
  {
    base64::decode(encoded, uncompressed.data(), uncompressed_bytes);
  }
  return binary::swap_if_needed(
      Read<T>(uncompressed.write()), is_little_endian);
}

void write_tag(std::ostream& stream, TagBase const* tag, Int space_dim) {
  if (!(tag->outflags() & OMEGA_H_DO_VIZ)) return;
  if (is<I8>(tag)) {
    write_array(stream, tag->name(), tag->ncomps(), to<I8>(tag)->array());
  } else if (is<I32>(tag)) {
    write_array(stream, tag->name(), tag->ncomps(), to<I32>(tag)->array());
  } else if (is<I64>(tag)) {
    write_array(stream, tag->name(), tag->ncomps(), to<I64>(tag)->array());
  } else if (is<Real>(tag)) {
    Reals array = to<Real>(tag)->array();
    if (space_dim == 2 && tag->ncomps() == space_dim) {
      // VTK / Paraview expect vector fields to have 3 components
      // regardless of whether this is a 2D mesh or not.
      // this filter adds a 3rd zero component to any
      // fields with 2 components for 2D meshes
      CHECK(array.exists());
      write_array(stream, tag->name(), 3, vectors_2d_to_3d(array));
    } else {
      write_array(stream, tag->name(), tag->ncomps(), array);
    }
  } else {
    Omega_h_fail("unknown tag type in write_tag");
  }
}

bool read_tag(std::istream& stream, Mesh* mesh, Int ent_dim,
    bool is_little_endian, bool is_compressed) {
  Omega_h_Type type = OMEGA_H_I8;
  std::string name;
  Int ncomps = -1;
  if (!read_array_start_tag(stream, &type, &name, &ncomps)) {
    return false;
  }
  /* tags like "global" are set by the construction mechanism,
     and it is somewhat complex to anticipate when they exist
     so we can just remove them if they are going to be reset. */
  if (mesh->has_tag(ent_dim, name)) mesh->remove_tag(ent_dim, name);
  auto size = mesh->nents(ent_dim) * ncomps;
  if (type == OMEGA_H_I8) {
    auto array = read_array<I8>(stream, size, is_little_endian, is_compressed);
    mesh->add_tag(ent_dim, name, ncomps, OMEGA_H_DONT_TRANSFER,
        OMEGA_H_DO_OUTPUT, array, true);
  } else if (type == OMEGA_H_I32) {
    auto array = read_array<I32>(stream, size, is_little_endian, is_compressed);
    mesh->add_tag(ent_dim, name, ncomps, OMEGA_H_DONT_TRANSFER,
        OMEGA_H_DO_OUTPUT, array, true);
  } else if (type == OMEGA_H_I64) {
    auto array = read_array<I64>(stream, size, is_little_endian, is_compressed);
    mesh->add_tag(ent_dim, name, ncomps, OMEGA_H_DONT_TRANSFER,
        OMEGA_H_DO_OUTPUT, array, true);
  } else {
    auto array =
        read_array<Real>(stream, size, is_little_endian, is_compressed);
    mesh->add_tag(ent_dim, name, ncomps, OMEGA_H_DONT_TRANSFER,
        OMEGA_H_DO_OUTPUT, array, true);
  }
  auto et = xml::read_tag(stream);
  CHECK(et.elem_name == "DataArray");
  CHECK(et.type == xml::Tag::END);
  return true;
}

template <typename T>
Read<T> read_known_array(std::istream& stream, std::string const& name,
    LO nents, Int ncomps, bool is_little_endian, bool is_compressed) {
  auto st = xml::read_tag(stream);
  CHECK(st.elem_name == "DataArray");
  CHECK(st.type == xml::Tag::START);
  CHECK(st.attribs["Name"] == name);
  CHECK(st.attribs["type"] == Traits<T>::name());
  CHECK(st.attribs["NumberOfComponents"] == to_string(ncomps));
  auto array =
      read_array<T>(stream, nents * ncomps, is_little_endian, is_compressed);
  auto et = xml::read_tag(stream);
  CHECK(et.elem_name == "DataArray");
  CHECK(et.type == xml::Tag::END);
  return array;
}

enum {
  VTK_VERTEX = 1,
  VTK_POLY_VERTEX = 2,
  VTK_LINE = 3,
  VTK_POLY_LINE = 4,
  VTK_TRIANGLE = 5,
  VTK_TRIANGLE_STRIP = 6,
  VTK_POLYGON = 7,
  VTK_PIXEL = 8,
  VTK_QUAD = 9,
  VTK_TETRA = 10,
  VTK_VOXEL = 11,
  VTK_HEXAHEDRON = 12,
  VTK_WEDGE = 13,
  VTK_PYRAMID = 14
};

static I8 const vtk_types[DIMS] = {
    VTK_VERTEX, VTK_LINE, VTK_TRIANGLE, VTK_TETRA};

static void write_vtkfile_vtu_start_tag(std::ostream& stream) {
  stream << "<VTKFile type=\"UnstructuredGrid\" byte_order=\"";
  if (is_little_endian_cpu())
    stream << "LittleEndian";
  else
    stream << "BigEndian";
  stream << "\" header_type=\"";
  static_assert(sizeof(std::size_t) == 8,
      "UInt32 Traits was removed to silence warnings");
  stream << Traits<std::size_t>::name();
  stream << "\"";
#ifdef OMEGA_H_USE_ZLIB
  stream << " compressor=\"vtkZLibDataCompressor\"";
#endif
  stream << ">\n";
}

static void read_vtkfile_vtu_start_tag(
    std::istream& stream, bool* is_little_endian_out, bool* is_compressed_out) {
  auto st = xml::read_tag(stream);
  CHECK(st.elem_name == "VTKFile");
  CHECK(st.attribs["header_type"] == Traits<std::size_t>::name());
  auto is_little_endian = (st.attribs["byte_order"] == "LittleEndian");
  *is_little_endian_out = is_little_endian;
  auto is_compressed = (st.attribs.count("compressor") == 1);
  *is_compressed_out = is_compressed;
}

void write_piece_start_tag(
    std::ostream& stream, Mesh const* mesh, Int cell_dim) {
  stream << "<Piece NumberOfPoints=\"" << mesh->nverts() << "\"";
  stream << " NumberOfCells=\"" << mesh->nents(cell_dim) << "\">\n";
}

void read_piece_start_tag(
    std::istream& stream, LO* nverts_out, LO* ncells_out) {
  auto st = xml::read_tag(stream);
  CHECK(st.elem_name == "Piece");
  *nverts_out = std::stoi(st.attribs["NumberOfPoints"]);
  *ncells_out = std::stoi(st.attribs["NumberOfCells"]);
}

void write_connectivity(std::ostream& stream, Mesh* mesh, Int cell_dim) {
  Read<I8> types(mesh->nents(cell_dim), vtk_types[cell_dim]);
  write_array(stream, "types", 1, types);
  LOs ev2v = mesh->ask_verts_of(cell_dim);
  LOs ends(mesh->nents(cell_dim), simplex_degrees[cell_dim][VERT],
      simplex_degrees[cell_dim][VERT]);
  write_array(stream, "connectivity", 1, ev2v);
  write_array(stream, "offsets", 1, ends);
}

void read_connectivity(std::istream& stream, CommPtr comm, LO ncells,
    bool is_little_endian, bool is_compressed, Int* dim_out, LOs* ev2v_out) {
  auto types = read_known_array<I8>(
      stream, "types", ncells, 1, is_little_endian, is_compressed);
  Int dim = -1;
  if (types.size()) {
    auto type = types.get(0);
    if (type == VTK_TRIANGLE) dim = 2;
    if (type == VTK_TETRA) dim = 3;
  }
  dim = comm->allreduce(dim, OMEGA_H_MAX);
  CHECK(dim == 2 || dim == 3);
  *dim_out = dim;
  auto ev2v = read_known_array<LO>(stream, "connectivity", ncells * (dim + 1),
      1, is_little_endian, is_compressed);
  *ev2v_out = ev2v;
  read_known_array<LO>(
      stream, "offsets", ncells, 1, is_little_endian, is_compressed);
}

void write_locals(std::ostream& stream, Mesh* mesh, Int ent_dim) {
  write_array(stream, "local", 1, Read<LO>(mesh->nents(ent_dim), 0, 1));
}

void write_owners(std::ostream& stream, Mesh* mesh, Int ent_dim) {
  if (mesh->comm()->size() == 1) return;
  write_array(stream, "owner", 1, mesh->ask_owners(ent_dim).ranks);
}

void write_locals_and_owners(std::ostream& stream, Mesh* mesh, Int ent_dim) {
  write_locals(stream, mesh, ent_dim);
  write_owners(stream, mesh, ent_dim);
}

void read_locals_and_owners(std::istream& stream, CommPtr comm, LO nents,
    bool is_little_endian, bool is_compressed) {
  read_known_array<LO>(
      stream, "local", nents, 1, is_little_endian, is_compressed);
  if (comm->size() == 1) return;
  read_known_array<I32>(
      stream, "owner", nents, 1, is_little_endian, is_compressed);
}

template <typename T>
void write_p_data_array(
    std::ostream& stream, std::string const& name, Int ncomps) {
  stream << "<PDataArray ";
  describe_array<T>(stream, name, ncomps);
  stream << "/>\n";
}

void write_p_data_array2(std::ostream& stream, std::string const& name,
    Int ncomps, Int Omega_h_Type) {
  switch (Omega_h_Type) {
    case OMEGA_H_I8:
      write_p_data_array<I8>(stream, name, ncomps);
      break;
    case OMEGA_H_I32:
      write_p_data_array<I32>(stream, name, ncomps);
      break;
    case OMEGA_H_I64:
      write_p_data_array<I64>(stream, name, ncomps);
      break;
    case OMEGA_H_F64:
      write_p_data_array<Real>(stream, name, ncomps);
      break;
  }
}

void write_p_tag(std::ostream& stream, TagBase const* tag, Int space_dim) {
  if (!(tag->outflags() & OMEGA_H_DO_VIZ)) return;
  if (tag->type() == OMEGA_H_F64 && tag->ncomps() == space_dim)
    write_p_data_array2(stream, tag->name(), 3, OMEGA_H_F64);
  else
    write_p_data_array2(stream, tag->name(), tag->ncomps(), tag->type());
}

std::string piece_filename(std::string const& piecepath, I32 rank) {
  return piecepath + '_' + to_string(rank) + ".vtu";
}

std::string get_rel_step_path(Int step) {
  return "steps/step_" + to_string(step);
}

std::string get_step_path(std::string const& root_path, Int step) {
  return root_path + '/' + get_rel_step_path(step);
}

}  // end anonymous namespace

std::string get_pvtu_path(std::string const& step_path) {
  return step_path + "/pieces.pvtu";
}

std::string get_pvd_path(std::string const& root_path) {
  return root_path + "/steps.pvd";
}

void write_vtu(std::ostream& stream, Mesh* mesh, Int cell_dim) {
  write_vtkfile_vtu_start_tag(stream);
  stream << "<UnstructuredGrid>\n";
  write_piece_start_tag(stream, mesh, cell_dim);
  stream << "<Cells>\n";
  write_connectivity(stream, mesh, cell_dim);
  stream << "</Cells>\n";
  stream << "<Points>\n";
  write_tag(stream, mesh->get_tag<Real>(VERT, "coordinates"), mesh->dim());
  stream << "</Points>\n";
  stream << "<PointData>\n";
  write_locals_and_owners(stream, mesh, VERT);
  if (mesh->has_tag(VERT, "global")) {
    write_tag(stream, mesh->get_tag<GO>(VERT, "global"), mesh->dim());
  }
  for (Int i = 0; i < mesh->ntags(VERT); ++i) {
    auto tag = mesh->get_tag(VERT, i);
    if (tag->name() != "coordinates" && tag->name() != "global") {
      write_tag(stream, tag, mesh->dim());
    }
  }
  stream << "</PointData>\n";
  stream << "<CellData>\n";
  write_locals_and_owners(stream, mesh, cell_dim);
  for (Int i = 0; i < mesh->ntags(cell_dim); ++i) {
    write_tag(stream, mesh->get_tag(cell_dim, i), mesh->dim());
  }
  stream << "</CellData>\n";
  stream << "</Piece>\n";
  stream << "</UnstructuredGrid>\n";
  stream << "</VTKFile>\n";
}

void read_vtu(std::istream& stream, CommPtr comm, Mesh* mesh) {
  bool is_little_endian, is_compressed;
  read_vtkfile_vtu_start_tag(stream, &is_little_endian, &is_compressed);
  CHECK(xml::read_tag(stream).elem_name == "UnstructuredGrid");
  LO nverts, ncells;
  read_piece_start_tag(stream, &nverts, &ncells);
  CHECK(xml::read_tag(stream).elem_name == "Cells");
  Int dim;
  LOs ev2v;
  read_connectivity(
      stream, comm, ncells, is_little_endian, is_compressed, &dim, &ev2v);
  CHECK(xml::read_tag(stream).elem_name == "Cells");
  CHECK(xml::read_tag(stream).elem_name == "Points");
  auto coords = read_known_array<Real>(
      stream, "coordinates", nverts, 3, is_little_endian, is_compressed);
  if (dim == 2) coords = vectors_3d_to_2d(coords);
  CHECK(xml::read_tag(stream).elem_name == "Points");
  CHECK(xml::read_tag(stream).elem_name == "PointData");
  read_locals_and_owners(stream, comm, nverts, is_little_endian, is_compressed);
  Read<GO> vert_globals;
  if (comm->size() > 1) {
    vert_globals = read_known_array<GO>(
        stream, "global", nverts, 1, is_little_endian, is_compressed);
  } else {
    vert_globals = Read<GO>(nverts, 0, 1);
  }
  build_from_elems2verts(mesh, comm, dim, ev2v, vert_globals);
  mesh->add_tag(VERT, "coordinates", dim, OMEGA_H_LINEAR_INTERP,
      OMEGA_H_DO_OUTPUT, coords, true);
  while (read_tag(stream, mesh, VERT, is_little_endian, is_compressed))
    ;
  CHECK(xml::read_tag(stream).elem_name == "CellData");
  read_locals_and_owners(stream, comm, ncells, is_little_endian, is_compressed);
  while (read_tag(stream, mesh, dim, is_little_endian, is_compressed))
    ;
  CHECK(xml::read_tag(stream).elem_name == "Piece");
  CHECK(xml::read_tag(stream).elem_name == "UnstructuredGrid");
  CHECK(xml::read_tag(stream).elem_name == "VTKFile");
}

void write_vtu(std::string const& filename, Mesh* mesh, Int cell_dim) {
  std::ofstream file(filename.c_str());
  CHECK(file.is_open());
  write_vtu(file, mesh, cell_dim);
}

void write_pvtu(std::ostream& stream, Mesh* mesh, Int cell_dim,
    std::string const& piecepath) {
  stream << "<VTKFile type=\"PUnstructuredGrid\">\n";
  stream << "<PUnstructuredGrid GhostLevel=\"0\">\n";
  stream << "<PPoints>\n";
  write_p_data_array<Real>(stream, "coordinates", 3);
  stream << "</PPoints>\n";
  stream << "<PPointData>\n";
  write_p_data_array2(stream, "local", 1, OMEGA_H_I32);
  if (mesh->comm()->size() > 1) {
    write_p_data_array2(stream, "owner", 1, OMEGA_H_I32);
  }
  if (mesh->has_tag(VERT, "global")) {
    write_p_data_array2(stream, "global", 1, OMEGA_H_I64);
  }
  for (Int i = 0; i < mesh->ntags(VERT); ++i) {
    auto tag = mesh->get_tag(VERT, i);
    if (tag->name() != "coordinates" && tag->name() != "global") {
      write_p_tag(stream, tag, mesh->dim());
    }
  }
  stream << "</PPointData>\n";
  stream << "<PCellData>\n";
  write_p_data_array2(stream, "local", 1, OMEGA_H_I32);
  if (mesh->comm()->size() > 1)
    write_p_data_array2(stream, "owner", 1, OMEGA_H_I32);
  for (Int i = 0; i < mesh->ntags(cell_dim); ++i) {
    write_p_tag(stream, mesh->get_tag(cell_dim, i), mesh->dim());
  }
  stream << "</PCellData>\n";
  for (I32 i = 0; i < mesh->comm()->size(); ++i) {
    stream << "<Piece Source=\"" << piece_filename(piecepath, i) << "\"/>\n";
  }
  stream << "</PUnstructuredGrid>\n";
  stream << "</VTKFile>\n";
}

void write_pvtu(std::string const& filename, Mesh* mesh, Int cell_dim,
    std::string const& piecepath) {
  std::ofstream file(filename.c_str());
  CHECK(file.is_open());
  write_pvtu(file, mesh, cell_dim, piecepath);
}

void read_pvtu(std::istream& stream, CommPtr comm, I32* npieces_out,
    std::string* vtupath_out) {
  I32 npieces = 0;
  std::string vtupath;
  for (std::string line; std::getline(stream, line);) {
    xml::Tag tag;
    if (!xml::parse_tag(line, &tag)) continue;
    if (tag.elem_name != "Piece") continue;
    if (npieces == comm->rank()) {
      vtupath = tag.attribs["Source"];
    }
    ++npieces;
  }
  CHECK(npieces >= 1);
  CHECK(npieces <= comm->size());
  *npieces_out = npieces;
  *vtupath_out = vtupath;
}

void read_pvtu(std::string const& pvtupath, CommPtr comm, I32* npieces_out,
    std::string* vtupath_out) {
  auto parentpath = parent_path(pvtupath);
  I32 npieces;
  std::string vtupath;
  std::ifstream stream(pvtupath.c_str());
  if (!stream.is_open()) {
    Omega_h_fail("couldn't open \"%s\"\n", pvtupath.c_str());
  }
  read_pvtu(stream, comm, &npieces, &vtupath);
  vtupath = parentpath + "/" + vtupath;
  *npieces_out = npieces;
  *vtupath_out = vtupath;
}

void write_parallel(std::string const& path, Mesh* mesh, Int cell_dim) {
  auto rank = mesh->comm()->rank();
  if (rank == 0) {
    safe_mkdir(path.c_str());
  }
  mesh->comm()->barrier();
  auto piecesdir = path + "/pieces";
  if (rank == 0) {
    safe_mkdir(piecesdir.c_str());
  }
  mesh->comm()->barrier();
  auto piecepath = piecesdir + "/piece";
  auto pvtuname = get_pvtu_path(path);
  if (rank == 0) {
    write_pvtu(pvtuname, mesh, cell_dim, "pieces/piece");
  }
  write_vtu(piece_filename(piecepath, rank), mesh, cell_dim);
}

void read_parallel(std::string const& pvtupath, CommPtr comm, Mesh* mesh) {
  I32 npieces;
  std::string vtupath;
  read_pvtu(pvtupath, comm, &npieces, &vtupath);
  bool in_subcomm = (comm->rank() < npieces);
  auto subcomm = comm->split(I32(!in_subcomm), 0);
  if (in_subcomm) {
    std::ifstream vtustream(vtupath.c_str());
    CHECK(vtustream.is_open());
    read_vtu(vtustream, subcomm, mesh);
  }
  mesh->set_comm(comm);
}

std::streampos write_initial_pvd(std::string const& root_path) {
  std::string pvdpath = get_pvd_path(root_path);
  std::ofstream file(pvdpath.c_str());
  CHECK(file.is_open());
  file << "<VTKFile type=\"Collection\" version=\"0.1\">\n";
  file << "<Collection>\n";
  auto pos = file.tellp();
  file << "</Collection>\n";
  file << "</VTKFile>\n";
  return pos;
}

void update_pvd(std::string const& root_path, std::streampos* pos_inout,
    Int step, Real time) {
  std::string pvdpath = get_pvd_path(root_path);
  std::fstream file;
  file.open(pvdpath.c_str(), std::ios::out | std::ios::in);
  CHECK(file.is_open());
  file.seekp(*pos_inout);
  file << "<DataSet timestep=\"" << time << "\" part=\"0\" ";
  auto relstep = get_rel_step_path(step);
  auto relpvtu = get_pvtu_path(relstep);
  file << "file=\"" << relpvtu << "\"/>\n";
  *pos_inout = file.tellp();
  file << "</Collection>\n";
  file << "</VTKFile>\n";
}

void read_pvd(std::istream& stream, std::vector<Real>* times_out,
    std::vector<std::string>* pvtupaths_out) {
  std::vector<Real> times;
  std::vector<std::string> pvtupaths;
  for (std::string line; std::getline(stream, line);) {
    xml::Tag tag;
    if (!xml::parse_tag(line, &tag)) continue;
    if (tag.elem_name != "DataSet") continue;
    times.push_back(std::stod(tag.attribs["timestep"]));
    pvtupaths.push_back(tag.attribs["file"]);
  }
  *times_out = times;
  *pvtupaths_out = pvtupaths;
}

void read_pvd(std::string const& pvdpath, std::vector<Real>* times_out,
    std::vector<std::string>* pvtupaths_out) {
  std::vector<Real> times;
  std::vector<std::string> pvtupaths;
  std::ifstream pvdstream(pvdpath.c_str());
  CHECK(pvdstream.is_open());
  read_pvd(pvdstream, &times, &pvtupaths);
  auto parentpath = parent_path(pvdpath);
  for (auto& pvtupath : pvtupaths) pvtupath = parentpath + "/" + pvtupath;
  *times_out = times;
  *pvtupaths_out = pvtupaths;
}

Writer::Writer()
    : mesh_(nullptr),
      root_path_("/not-set"),
      cell_dim_(-1),
      step_(-1),
      pvd_pos_(0) {}

Writer::Writer(Writer const& other)
    : mesh_(other.mesh_),
      root_path_(other.root_path_),
      cell_dim_(other.cell_dim_),
      step_(other.step_),
      pvd_pos_(other.pvd_pos_) {}

Writer& Writer::operator=(Writer const& other) {
  mesh_ = other.mesh_;
  root_path_ = other.root_path_;
  cell_dim_ = other.cell_dim_;
  step_ = other.step_;
  pvd_pos_ = other.pvd_pos_;
  return *this;
}

Writer::~Writer() {}

Writer::Writer(Mesh* mesh, std::string const& root_path, Int cell_dim)
    : mesh_(mesh),
      root_path_(root_path),
      cell_dim_(cell_dim),
      step_(0),
      pvd_pos_(0) {
  auto comm = mesh->comm();
  auto rank = comm->rank();
  if (rank == 0) safe_mkdir(root_path_.c_str());
  comm->barrier();
  auto stepsdir = root_path_ + "/steps";
  if (rank == 0) safe_mkdir(stepsdir.c_str());
  comm->barrier();
  if (rank == 0) {
    pvd_pos_ = write_initial_pvd(root_path);
  }
}

void Writer::write(Real time) {
  write_parallel(get_step_path(root_path_, step_), mesh_, cell_dim_);
  if (mesh_->comm()->rank() == 0) {
    update_pvd(root_path_, &pvd_pos_, step_, time);
  }
  ++step_;
}

void Writer::write() { this->write(Real(step_)); }

FullWriter::FullWriter() {}

FullWriter::FullWriter(FullWriter const& other) : writers_(other.writers_) {}

FullWriter& FullWriter::operator=(FullWriter const& other) {
  writers_ = other.writers_;
  return *this;
}

FullWriter::~FullWriter() {}

FullWriter::FullWriter(Mesh* mesh, std::string const& root_path) {
  auto comm = mesh->comm();
  auto rank = comm->rank();
  if (rank == 0) safe_mkdir(root_path.c_str());
  comm->barrier();
  for (Int i = EDGE; i <= mesh->dim(); ++i)
    writers_.push_back(Writer(mesh, root_path + "/" + plural_names[i], i));
}

void FullWriter::write(Real time) {
  for (auto& writer : writers_) writer.write(time);
}

void FullWriter::write() {
  for (auto& writer : writers_) writer.write();
}

}  // end namespace vtk

}  // end namespace Omega_h
