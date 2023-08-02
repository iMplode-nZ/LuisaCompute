#pragma once
#include <cstdint>
#include "dense_storage_view.h"

namespace luisa::compute::tensor {
enum class DenseMatrixShape {
    GENERAL = 0,   // general matrix*
    TRIANGULAR = 1,// triangular matrix

    BAND = 2,           // band matrix
    TRIANGULAR_BAND = 3,// triangular band

    PACKED_TRIANGULAR = 4,// packed triangular
    PACKED = 5            // packed: only for packed symmetric matrix
};

enum class DenseMatrixProperty {
    NONE = 0,     // none
    SYMMETRIC = 1,// symmetric matrix
};

enum class DenseMatrixFillMode {
    NONE = 0,
    LOWER = 1,// lower triangular part
    UPPER = 2,// upper triangular part
};

enum class DenseMatrixDiagType {
    NON_UNIT = 0,
    UNIT = 1
};

class DenseMatrixDesc {
public:
    int offset;// start offset
    int row, col;
    int lda;   // leading dimension of two-dimensional array used to store matrix A
    int kl, ku;// for band matrix
    DenseMatrixShape shape;
    DenseMatrixProperty property;
    DenseMatrixFillMode fill_mode;
    DenseMatrixDiagType diag_type;
};

class DenseMatrixView {
public:
    DenseStorageView storage;
    DenseMatrixDesc desc;
    MatrixOperation operation;
};
}// namespace luisa::compute::tensor