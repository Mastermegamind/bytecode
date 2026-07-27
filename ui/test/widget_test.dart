import 'package:flutter_test/flutter_test.dart';
import 'package:bytecode_encoder/main.dart';

void main() {
  testWidgets('encoder shell renders primary controls', (tester) async {
    await tester.pumpWidget(const BytecodeApp());

    expect(find.text('Bytecode Encoder'), findsOneWidget);
    expect(find.text('Add Files'), findsOneWidget);
    expect(find.text('Add Folder'), findsOneWidget);
    expect(find.text('Output folder'), findsOneWidget);
    expect(find.text('Vendor secret key file'), findsOneWidget);
    expect(find.text('Dump'), findsOneWidget);
    expect(find.text('Verify'), findsOneWidget);
  });
}
